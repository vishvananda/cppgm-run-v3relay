#include "lowir_validate.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using lowir_model::Block;
using lowir_model::Function;
using lowir_model::FunctionBoundaryMetadata;
using lowir_model::FunctionDeclaration;
using lowir_model::GlobalDefinition;
using lowir_model::Instruction;
using lowir_model::LowType;
using lowir_model::LowTypeInfo;
using lowir_model::Operand;
using lowir_model::Parameter;
using lowir_model::ParameterMetadata;
using lowir_model::Program;
using lowir_model::SymbolMetadata;

struct SymbolIndex
{
  std::unordered_set<std::string> symbols;
  std::unordered_set<std::string> globals;
  std::unordered_set<std::string> functions;
  std::unordered_set<std::string> thread_local_globals;
  std::unordered_map<std::string, LowType> global_types;
  std::unordered_map<std::string, const Function *> function_definitions;
  std::unordered_map<std::string, const FunctionDeclaration *> function_declarations;
};

struct FunctionEnvironment
{
  std::unordered_map<std::string, LowType> temporaries;
  std::unordered_map<std::string, LowType> slots;
  std::unordered_set<std::string> blocks;
};

bool target_is_thread_local(const SymbolIndex & index, const std::string & name);
void validate_operand_references_for_instruction(const Instruction & instruction,
                                                 const SymbolIndex & index,
                                                 const FunctionEnvironment & environment);

void fail(const std::string & message)
{
  throw lowir_model::ParseError(message);
}

LowType make_type(const char * text)
{
  LowType type;
  type.text = text;
  return type;
}

bool is_type(const LowType & type, const char * text)
{
  return type.text == text;
}

bool is_known(const LowType & type)
{
  return !type.text.empty() && lowir_model::describe_low_type(type).valid();
}

bool is_terminator(Instruction::Kind kind)
{
  return kind == Instruction::IK_JUMP || kind == Instruction::IK_BRANCH ||
         kind == Instruction::IK_SWITCH || kind == Instruction::IK_RETURN ||
         kind == Instruction::IK_THROW || kind == Instruction::IK_RESUME;
}

bool has_global(const SymbolIndex & index, const std::string & name)
{
  return index.globals.find(name) != index.globals.end();
}

bool has_function(const SymbolIndex & index, const std::string & name)
{
  return index.functions.find(name) != index.functions.end();
}

void record_symbol(SymbolIndex & index, const std::string & name, bool global)
{
  if(!index.symbols.insert(name).second) {
    fail("duplicate top-level symbol " + name);
  }
  if(global) index.globals.insert(name);
  else index.functions.insert(name);
}

void index_top_level_symbols(const Program & program, SymbolIndex & index)
{
  for(std::size_t i = 0; i < program.global_declarations.size(); ++i) {
    const lowir_model::GlobalDeclaration & declaration = program.global_declarations[i];
    record_symbol(index, declaration.name, true);
    if(declaration.has_type) index.global_types[declaration.name] = declaration.type;
    if(declaration.storage == lowir_model::GSM_THREAD_LOCAL) {
      index.thread_local_globals.insert(declaration.name);
    }
  }
  for(std::size_t i = 0; i < program.globals.size(); ++i) {
    const GlobalDefinition & global = program.globals[i];
    record_symbol(index, global.name, true);
    if(!global.structured) index.global_types[global.name] = global.type;
    if(global.storage == lowir_model::GSM_THREAD_LOCAL) {
      index.thread_local_globals.insert(global.name);
    }
  }
  for(std::size_t i = 0; i < program.function_declarations.size(); ++i) {
    const FunctionDeclaration & declaration = program.function_declarations[i];
    record_symbol(index, declaration.name, false);
    index.function_declarations[declaration.name] = &declaration;
  }
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    const Function & function = program.functions[i];
    record_symbol(index, function.name, false);
    index.function_definitions[function.name] = &function;
  }
}

void validate_aliases(const Program & program, const SymbolIndex & index)
{
  std::unordered_set<std::string> aliases;
  for(std::size_t i = 0; i < program.object_aliases.size(); ++i) {
    const lowir_model::ObjectAlias & alias = program.object_aliases[i];
    if(!aliases.insert(alias.object_symbol).second) {
      fail("duplicate object alias " + alias.object_symbol);
    }
    if(index.symbols.find(alias.target) == index.symbols.end()) {
      fail("object alias target is undefined");
    }
  }
}

void validate_tls_metadata(const Program & program, const SymbolIndex & index)
{
  std::unordered_set<std::string> wrappers;
  for(std::size_t i = 0; i < program.global_declarations.size(); ++i) {
    if(!program.global_declarations[i].metadata.tls_for_symbol.empty()) {
      fail("tls_for is only valid on functions");
    }
  }
  for(std::size_t i = 0; i < program.globals.size(); ++i) {
    if(!program.globals[i].metadata.tls_for_symbol.empty()) {
      fail("tls_for is only valid on functions");
    }
  }
  for(std::size_t i = 0; i < program.function_declarations.size(); ++i) {
    const SymbolMetadata & metadata = program.function_declarations[i].metadata;
    if(metadata.tls_for_symbol.empty()) continue;
    if(!has_global(index, metadata.tls_for_symbol)) {
      fail("tls_for does not name a global");
    }
    const std::string & target = metadata.tls_for_symbol;
    const bool thread_local_global = target_is_thread_local(index, target);
    if(!thread_local_global) fail("tls_for target is not thread-local");
    if(!wrappers.insert(target).second) fail("duplicate thread-local wrapper");
  }
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    const SymbolMetadata & metadata = program.functions[i].metadata;
    if(metadata.tls_for_symbol.empty()) continue;
    if(!has_global(index, metadata.tls_for_symbol) ||
       !target_is_thread_local(index, metadata.tls_for_symbol)) {
      fail("tls_for target is not thread-local");
    }
    if(!wrappers.insert(metadata.tls_for_symbol).second) {
      fail("duplicate thread-local wrapper");
    }
  }
}

bool target_is_thread_local(const SymbolIndex & index, const std::string & name)
{
  return index.thread_local_globals.find(name) != index.thread_local_globals.end();
}

void count_role(int & count, const char * role)
{
  ++count;
  if(count > 1) fail(std::string("duplicate ") + role + " role");
}

void validate_roles(const Program & program, LowirProgramFacts & facts)
{
  int entry_count = 0;
  int init_count = 0;
  int fini_count = 0;
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    const Function & function = program.functions[i];
    lowir_model::SymbolRole role = function.metadata.role;
    if(role == lowir_model::SR_NONE) {
      if(function.name == "@main") role = lowir_model::SR_ENTRY;
      else if(function.name == "@__cppgm_init") role = lowir_model::SR_INIT;
      else if(function.name == "@__cppgm_fini") role = lowir_model::SR_FINI;
    }
    if(role == lowir_model::SR_ENTRY) {
      count_role(entry_count, "entry");
      facts.entry_function = function.name;
      facts.entry = static_cast<int>(i);
    } else if(role == lowir_model::SR_INIT) {
      count_role(init_count, "init");
      facts.init = static_cast<int>(i);
    } else if(role == lowir_model::SR_FINI) {
      count_role(fini_count, "fini");
      facts.fini = static_cast<int>(i);
    }
  }
  if(entry_count != 1) fail("LowIR program needs exactly one entry function");
}

void ValidateTopLevelSymbols(const Program & program, SymbolIndex & index,
                             LowirProgramFacts & facts)
{
  index_top_level_symbols(program, index);
  for(std::size_t i = 0; i < program.global_declarations.size(); ++i) {
    LowirProgramFacts::SymbolRef ref;
    ref.kind = LowirProgramFacts::SymbolRef::DECL_GLOBAL;
    ref.index = i;
    facts.symbols[program.global_declarations[i].name] = ref;
  }
  for(std::size_t i = 0; i < program.globals.size(); ++i) {
    LowirProgramFacts::SymbolRef ref;
    ref.kind = LowirProgramFacts::SymbolRef::DEF_GLOBAL;
    ref.index = i;
    facts.symbols[program.globals[i].name] = ref;
  }
  for(std::size_t i = 0; i < program.function_declarations.size(); ++i) {
    LowirProgramFacts::SymbolRef ref;
    ref.kind = LowirProgramFacts::SymbolRef::DECL_FUNCTION;
    ref.index = i;
    facts.symbols[program.function_declarations[i].name] = ref;
  }
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    LowirProgramFacts::SymbolRef ref;
    ref.kind = LowirProgramFacts::SymbolRef::DEF_FUNCTION;
    ref.index = i;
    facts.symbols[program.functions[i].name] = ref;
  }
  validate_aliases(program, index);
  validate_tls_metadata(program, index);
  validate_roles(program, facts);
}

bool function_role(lowir_model::SymbolRole role)
{
  return role == lowir_model::SR_ENTRY || role == lowir_model::SR_INIT ||
         role == lowir_model::SR_FINI || role >= lowir_model::SR_EH_UNHANDLED;
}

bool global_role(lowir_model::SymbolRole role)
{
  return role == lowir_model::SR_EH_TOP || role == lowir_model::SR_EH_VALUE ||
         role == lowir_model::SR_EH_TYPE;
}

void count_explicit_role(lowir_model::SymbolRole role, int * counts)
{
  if(role == lowir_model::SR_NONE) return;
  const int value = static_cast<int>(role);
  ++counts[value];
  if(counts[value] > 1) fail("duplicate singleton symbol role");
}

void validate_role_domains(const Program & program)
{
  int role_counts[lowir_model::SR_EH_RESUME + 1] = { 0 };
  for(std::size_t i = 0; i < program.global_declarations.size(); ++i) {
    const lowir_model::SymbolRole role = program.global_declarations[i].metadata.role;
    if(role != lowir_model::SR_NONE && !global_role(role)) {
      fail("function role attached to a global");
    }
    count_explicit_role(role, role_counts);
  }
  for(std::size_t i = 0; i < program.globals.size(); ++i) {
    const lowir_model::SymbolRole role = program.globals[i].metadata.role;
    if(role != lowir_model::SR_NONE && !global_role(role)) {
      fail("function role attached to a global");
    }
    count_explicit_role(role, role_counts);
  }
  for(std::size_t i = 0; i < program.function_declarations.size(); ++i) {
    const lowir_model::SymbolRole role = program.function_declarations[i].metadata.role;
    if(role != lowir_model::SR_NONE && !function_role(role)) {
      fail("global role attached to a function");
    }
    count_explicit_role(role, role_counts);
  }
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    const lowir_model::SymbolRole role = program.functions[i].metadata.role;
    if(role != lowir_model::SR_NONE && !function_role(role)) {
      fail("global role attached to a function");
    }
    count_explicit_role(role, role_counts);
  }
}

void validate_parameter_metadata(const std::vector<Parameter> & params,
                                const LowType & return_type,
                                const std::string & boundary_name)
{
  std::unordered_set<std::string> names;
  for(std::size_t i = 0; i < params.size(); ++i) {
    const Parameter & parameter = params[i];
    if(!names.insert(parameter.name).second) {
      fail("duplicate parameter name in " + boundary_name);
    }
    const LowTypeInfo info = lowir_model::describe_low_type(parameter.type);
    if(!info.valid() || info.kind == LowTypeInfo::LTI_VOID) {
      fail("invalid parameter type");
    }
    const ParameterMetadata & metadata = parameter.metadata;
    if(metadata.passing != lowir_model::PPM_DIRECT && !info.pointer()) {
      fail("non-pointer parameter has indirect passing metadata");
    }
    if(metadata.capture != lowir_model::PCM_DEFAULT && !info.pointer()) {
      fail("non-pointer parameter has capture metadata");
    }
    if(metadata.access != lowir_model::PAM_DEFAULT && !info.pointer()) {
      fail("non-pointer parameter has access metadata");
    }
    if(metadata.alias != lowir_model::PALM_DEFAULT && !info.pointer()) {
      fail("non-pointer parameter has alias metadata");
    }
    if(metadata.passing == lowir_model::PPM_INDIRECT_RESULT && i != 0) {
      fail("indirect result parameter must be first");
    }
    if(metadata.passing == lowir_model::PPM_INDIRECT_RESULT &&
       !is_type(return_type, "void")) {
      fail("indirect result requires a void return type");
    }
  }
}

void validate_debug_location(const lowir_model::InstructionDebugLocation & location)
{
  if(!location.file.empty() && (location.line == 0 || location.column == 0)) {
    fail("debug location line and column must be positive");
  }
  if(location.file.empty() && (location.line != 0 || location.column != 0)) {
    fail("debug location needs a source file");
  }
}

void ValidateMetadata(const Program & program)
{
  validate_role_domains(program);
  for(std::size_t i = 0; i < program.function_declarations.size(); ++i) {
    const FunctionDeclaration & declaration = program.function_declarations[i];
    validate_parameter_metadata(declaration.params, declaration.return_type,
                                declaration.name);
  }
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    const Function & function = program.functions[i];
    validate_parameter_metadata(function.params, function.return_type, function.name);
    validate_debug_location(function.debug_location);
  }
}

void validate_global_initializer(const GlobalDefinition & global,
                                const SymbolIndex & index)
{
  const LowTypeInfo info = lowir_model::describe_low_type(global.type);
  if(!info.valid() || info.kind == LowTypeInfo::LTI_VOID) {
    fail("global definition has an invalid type");
  }
  if(global.init_kind == GlobalDefinition::INIT_ADDR) {
    if(!info.pointer() || index.symbols.find(global.init_operand.text) == index.symbols.end()) {
      fail("global address initializer is invalid");
    }
  } else if(global.init_kind == GlobalDefinition::INIT_INTEGER &&
            global.init_operand.literal_type.text != global.type.text) {
    fail("global initializer type mismatch");
  }
}

void validate_structured_global(const GlobalDefinition & global,
                                const SymbolIndex & index)
{
  if(global.data_items.empty()) fail("structured global is empty");
  for(std::size_t i = 0; i < global.data_items.size(); ++i) {
    const GlobalDefinition::DataItem & item = global.data_items[i];
    if(item.kind == GlobalDefinition::DataItem::ITEM_ZERO) {
      if(item.zero_bytes == 0) fail("structured global zero span must be positive");
      continue;
    }
    const LowTypeInfo info = lowir_model::describe_low_type(item.type);
    if(!info.valid() || info.kind == LowTypeInfo::LTI_VOID || info.object()) {
      fail("invalid structured global item type");
    }
    if(item.kind == GlobalDefinition::DataItem::ITEM_ADDR) {
      if(!info.pointer() || index.symbols.find(item.symbol) == index.symbols.end()) {
        fail("structured global address initializer is invalid");
      }
    } else if(item.literal_operand.literal_type.text != item.type.text) {
      fail("structured global initializer type mismatch");
    }
  }
}

void ValidateGlobals(const Program & program, const SymbolIndex & index)
{
  for(std::size_t i = 0; i < program.global_declarations.size(); ++i) {
    const lowir_model::GlobalDeclaration & declaration = program.global_declarations[i];
    if(declaration.has_type && !is_known(declaration.type)) {
      fail("invalid global declaration type");
    }
  }
  for(std::size_t i = 0; i < program.globals.size(); ++i) {
    const GlobalDefinition & global = program.globals[i];
    if(global.structured) validate_structured_global(global, index);
    else validate_global_initializer(global, index);
  }
}

void validate_operand_reference(const Operand & operand,
                                const SymbolIndex & index,
                                const FunctionEnvironment & environment)
{
  if(operand.kind == Operand::OP_TEMP &&
     environment.temporaries.find(operand.text) == environment.temporaries.end()) {
    fail("use of undefined temporary " + operand.text);
  }
  if(operand.kind == Operand::OP_SLOT &&
     environment.slots.find(operand.text) == environment.slots.end()) {
    fail("use of undefined slot " + operand.text);
  }
  if(operand.kind == Operand::OP_GLOBAL &&
     index.symbols.find(operand.text) == index.symbols.end()) {
    fail("use of undefined global/function " + operand.text);
  }
  if(operand.kind == Operand::OP_LABEL) {
    fail("block label is not a value");
  }
}

LowType operand_type(const Operand & operand, const SymbolIndex & index,
                     const FunctionEnvironment & environment)
{
  validate_operand_reference(operand, index, environment);
  if(operand.kind == Operand::OP_TEMP) return environment.temporaries.at(operand.text);
  if(operand.kind == Operand::OP_SLOT) return environment.slots.at(operand.text);
  if(operand.kind == Operand::OP_GLOBAL) {
    if(has_function(index, operand.text)) return make_type("ptr");
    const std::unordered_map<std::string, LowType>::const_iterator found =
      index.global_types.find(operand.text);
    return found == index.global_types.end() ? LowType() : found->second;
  }
  if(operand.kind == Operand::OP_INTEGER) {
    return operand.literal_type.text.empty() ? make_type("i64") : operand.literal_type;
  }
  if(operand.kind == Operand::OP_FLOAT) {
    return operand.literal_type.text.empty() ? make_type("f64") : operand.literal_type;
  }
  return LowType();
}

bool is_null_pointer_literal(const Operand & operand)
{
  return operand.kind == Operand::OP_INTEGER &&
         (operand.text == "nullptr" || operand.text == "0");
}

bool compatible(const LowType & expected, const LowType & actual,
                const Operand & operand, bool slot_is_address)
{
  if((operand.kind == Operand::OP_INTEGER || operand.kind == Operand::OP_FLOAT) &&
     operand.literal_type.text.empty()) {
    return true;
  }
  if(!is_known(actual) || expected.text.empty()) return true;
  if(expected.text == actual.text) return true;
  if(expected.text == "ptr" && is_null_pointer_literal(operand)) return true;
  if(expected.text == "ptr" && operand.kind == Operand::OP_SLOT && slot_is_address) {
    return true;
  }
  return false;
}

void check_value_type(const Operand & operand, const LowType & expected,
                     const SymbolIndex & index, const FunctionEnvironment & environment,
                     bool slot_is_address = false)
{
  const LowType actual = operand_type(operand, index, environment);
  if(!compatible(expected, actual, operand, slot_is_address)) {
    fail("LowIR operand type mismatch");
  }
}

void check_pointer_value(const Operand & operand, const SymbolIndex & index,
                        const FunctionEnvironment & environment)
{
  validate_operand_reference(operand, index, environment);
  if(operand.kind == Operand::OP_SLOT) return;
  const LowType actual = operand_type(operand, index, environment);
  if(!actual.text.empty() && !is_type(actual, "ptr") && !lowir_model::describe_low_type(actual).object()) {
    fail("expected a pointer-valued operand");
  }
}

void check_storage(const Operand & operand, const LowType & stored_type,
                   const SymbolIndex & index, const FunctionEnvironment & environment)
{
  validate_operand_reference(operand, index, environment);
  if(operand.kind == Operand::OP_SLOT) {
    check_value_type(operand, stored_type, index, environment);
  } else if(operand.kind == Operand::OP_GLOBAL) {
    if(!has_global(index, operand.text)) fail("function is not storage");
    const std::unordered_map<std::string, LowType>::const_iterator found =
      index.global_types.find(operand.text);
    if(found != index.global_types.end() && found->second.text != stored_type.text) {
      fail("global storage type mismatch");
    }
  } else {
    check_pointer_value(operand, index, environment);
  }
}

LowType instruction_result_type(const Instruction & instruction)
{
  switch(instruction.kind) {
    case Instruction::IK_ADDR:
    case Instruction::IK_INDEX:
    case Instruction::IK_STACK_ALLOC:
      return make_type("ptr");
    case Instruction::IK_CMP:
      return make_type("i64");
    case Instruction::IK_CALL:
      return instruction.type;
    case Instruction::IK_VA_START:
      return make_type("ptr");
    default:
      return instruction.type;
  }
}

void collect_temporary_definitions(const Function & function,
                                   FunctionEnvironment & environment)
{
  for(std::size_t i = 0; i < function.params.size(); ++i) {
    if(!environment.temporaries.insert(std::make_pair(function.params[i].name,
                                                      function.params[i].type)).second) {
      fail("duplicate temporary parameter");
    }
  }
  for(std::size_t i = 0; i < function.slots.size(); ++i) {
    if(!environment.slots.insert(function.slots[i]).second) {
      fail("duplicate stack slot");
    }
  }
  for(std::size_t i = 0; i < function.blocks.size(); ++i) {
    if(!environment.blocks.insert(function.blocks[i].label).second) {
      fail("duplicate block label");
    }
    for(std::size_t j = 0; j < function.blocks[i].instructions.size(); ++j) {
      const Instruction & instruction = function.blocks[i].instructions[j];
      if(instruction.dest.empty()) continue;
      if(!environment.temporaries.insert(std::make_pair(
             instruction.dest, instruction_result_type(instruction))).second) {
        fail("duplicate temporary definition");
      }
    }
  }
}

void validate_target(const Operand & operand, const FunctionEnvironment & environment)
{
  if(operand.kind != Operand::OP_LABEL || environment.blocks.find(operand.text) == environment.blocks.end()) {
    fail("undefined block target");
  }
}

void validate_control_flow(const Instruction & instruction,
                           const FunctionEnvironment & environment)
{
  if(instruction.kind == Instruction::IK_JUMP) {
    validate_target(instruction.first, environment);
  } else if(instruction.kind == Instruction::IK_BRANCH) {
    validate_target(instruction.second, environment);
    validate_target(instruction.third, environment);
  } else if(instruction.kind == Instruction::IK_SWITCH) {
    validate_target(instruction.second, environment);
    if(instruction.args.size() % 2 != 0) fail("malformed switch arms");
    for(std::size_t i = 1; i < instruction.args.size(); i += 2) {
      validate_target(instruction.args[i], environment);
    }
  } else if(instruction.kind == Instruction::IK_EH_TRY ||
            instruction.kind == Instruction::IK_EH_CLEANUP) {
    validate_target(instruction.first, environment);
  }
}

void validate_span(const Instruction & instruction)
{
  if(instruction.byte_count == 0 || instruction.byte_alignment == 0 ||
     (instruction.byte_alignment & (instruction.byte_alignment - 1)) != 0) {
    fail("bulk memory span has invalid alignment");
  }
}

void validate_unary(const Instruction & instruction, const SymbolIndex & index,
                    const FunctionEnvironment & environment)
{
  const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
  if(instruction.op == "decay") {
    if(!info.pointer()) fail("decay requires a pointer type");
  } else if(instruction.op == "bswap") {
    if(!is_type(instruction.type, "i16") && !is_type(instruction.type, "i32") &&
       !is_type(instruction.type, "i64")) fail("invalid bswap type");
  } else if(instruction.op == "neg") {
    if(!info.integer() && !info.floating()) fail("neg requires a numeric type");
  } else if(instruction.op == "not" || instruction.op == "bitnot") {
    if(!info.integer()) fail("integer unary operation needs an integer type");
  } else {
    fail("unknown unary operation");
  }
  check_value_type(instruction.first, instruction.type, index, environment);
}

bool valid_integer_binary(const std::string & op)
{
  return op == "add" || op == "sub" || op == "mul" || op == "div" ||
         op == "mod" || op == "udiv" || op == "umod" || op == "and" ||
         op == "or" || op == "xor" || op == "shl" || op == "shr" ||
         op == "ushr";
}

bool valid_float_binary(const std::string & op)
{
  return op == "add" || op == "sub" || op == "mul" || op == "div";
}

void validate_binary(const Instruction & instruction, const SymbolIndex & index,
                     const FunctionEnvironment & environment)
{
  const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
  if(info.integer()) {
    if(!valid_integer_binary(instruction.op)) fail("invalid integer binary operation");
  } else if(info.floating()) {
    if(!valid_float_binary(instruction.op)) fail("invalid floating binary operation");
  } else {
    fail("binary operation needs a numeric type");
  }
  check_value_type(instruction.first, instruction.type, index, environment);
  check_value_type(instruction.second, instruction.type, index, environment);
}

bool valid_compare(const std::string & op)
{
  return op == "eq" || op == "ne" || op == "lt" || op == "le" || op == "gt" ||
         op == "ge" || op == "ult" || op == "ule" || op == "ugt" || op == "uge";
}

void validate_compare(const Instruction & instruction, const SymbolIndex & index,
                      const FunctionEnvironment & environment)
{
  const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
  if(!valid_compare(instruction.op) || info.kind == LowTypeInfo::LTI_VOID ||
     info.object()) fail("invalid comparison");
  if(info.pointer() && instruction.op != "eq" && instruction.op != "ne") {
    fail("ordered pointer comparison is not supported");
  }
  check_value_type(instruction.first, instruction.type, index, environment);
  check_value_type(instruction.second, instruction.type, index, environment);
}

void validate_conversion(const Instruction & instruction, const SymbolIndex & index,
                         const FunctionEnvironment & environment)
{
  const LowTypeInfo dst = lowir_model::describe_low_type(instruction.type);
  const LowTypeInfo src = lowir_model::describe_low_type(instruction.source_type);
  bool valid = false;
  if(instruction.op == "zext" || instruction.op == "sext") {
    valid = dst.integer() && src.integer() && dst.bits > src.bits;
  } else if(instruction.op == "trunc") {
    valid = dst.integer() && src.integer() && dst.bits < src.bits;
  } else if(instruction.op == "fpext") {
    valid = dst.floating() && src.floating() && dst.bits > src.bits;
  } else if(instruction.op == "fptrunc") {
    valid = dst.floating() && src.floating() && dst.bits < src.bits;
  } else if(instruction.op == "sitofp" || instruction.op == "uitofp") {
    valid = dst.floating() && src.integer();
  } else if(instruction.op == "fptosi" || instruction.op == "fptoui") {
    valid = dst.integer() && src.floating();
  }
  if(!valid) fail("invalid LowIR conversion");
  check_value_type(instruction.first, instruction.source_type, index, environment);
}

void validate_call(const Instruction & instruction, const SymbolIndex & index,
                   const FunctionEnvironment & environment, bool assigned)
{
  const FunctionDeclaration * declaration = 0;
  const Function * definition = 0;
  if(instruction.first.kind == Operand::OP_GLOBAL) {
    if(!has_function(index, instruction.first.text)) fail("call target is not a function");
    const std::unordered_map<std::string, const FunctionDeclaration *>::const_iterator d =
      index.function_declarations.find(instruction.first.text);
    const std::unordered_map<std::string, const Function *>::const_iterator f =
      index.function_definitions.find(instruction.first.text);
    if(d != index.function_declarations.end()) declaration = d->second;
    if(f != index.function_definitions.end()) definition = f->second;
  } else {
    check_pointer_value(instruction.first, index, environment);
    if(!instruction.has_call_signature) fail("indirect call needs an explicit signature");
  }

  if(assigned && instruction.call_returns_void) fail("void call cannot define a temporary");
  const std::vector<Parameter> * params = 0;
  FunctionBoundaryMetadata boundary;
  if(instruction.has_call_signature) {
    params = &instruction.call_params;
    boundary = instruction.call_boundary;
    validate_parameter_metadata(*params, instruction.call_return_type, "call signature");
    if(instruction.call_return_type.text != instruction.type.text) {
      fail("call signature return type mismatch");
    }
  } else if(declaration != 0) {
    params = &declaration->params;
    boundary = declaration->boundary;
    if(instruction.type.text != declaration->return_type.text) {
      fail("call return type mismatch");
    }
  } else if(definition != 0) {
    params = &definition->params;
    boundary = definition->boundary;
    if(instruction.type.text != definition->return_type.text) {
      fail("call return type mismatch");
    }
  }

  if(params == 0) return;
  const bool relaxed = boundary.arity == lowir_model::CAM_VARIADIC ||
                       boundary.arity == lowir_model::CAM_PROTOTYPE_RELAXED;
  if((!relaxed && instruction.args.size() != params->size()) ||
     (relaxed && instruction.args.size() < params->size())) {
    fail("call argument count mismatch");
  }
  for(std::size_t i = 0; i < params->size(); ++i) {
    check_value_type(instruction.args[i], (*params)[i].type, index, environment, true);
  }
}

void validate_memory_instruction(const Instruction & instruction, const SymbolIndex & index,
                                 const FunctionEnvironment & environment)
{
  switch(instruction.kind) {
    case Instruction::IK_LOAD:
      check_storage(instruction.first, instruction.type, index, environment);
      break;
    case Instruction::IK_ATOMIC_LOAD:
      check_pointer_value(instruction.first, index, environment);
      break;
    case Instruction::IK_STORE:
      check_value_type(instruction.first, instruction.type, index, environment);
      check_storage(instruction.second, instruction.type, index, environment);
      break;
    case Instruction::IK_ATOMIC_STORE:
      check_value_type(instruction.first, instruction.type, index, environment);
      check_pointer_value(instruction.second, index, environment);
      break;
    case Instruction::IK_ATOMIC_EXCHANGE:
    case Instruction::IK_ATOMIC_ADD_FETCH:
      check_pointer_value(instruction.first, index, environment);
      check_value_type(instruction.second, instruction.type, index, environment);
      break;
    case Instruction::IK_ATOMIC_COMPARE_EXCHANGE:
      check_pointer_value(instruction.first, index, environment);
      check_pointer_value(instruction.second, index, environment);
      check_value_type(instruction.third, instruction.type, index, environment);
      break;
    case Instruction::IK_INDEX:
      check_pointer_value(instruction.first, index, environment);
      check_value_type(instruction.second, make_type("i64"), index, environment);
      break;
    case Instruction::IK_COPYOBJ:
      validate_span(instruction);
      check_pointer_value(instruction.second, index, environment);
      {
        const LowType source = operand_type(instruction.first, index, environment);
        const LowTypeInfo source_info = lowir_model::describe_low_type(source);
        if(source_info.object()) {
          if(source_info.bytes != instruction.byte_count ||
             source_info.alignment != instruction.byte_alignment) {
            fail("object copy span does not match source object");
          }
        } else {
          check_pointer_value(instruction.first, index, environment);
        }
      }
      break;
    case Instruction::IK_ZEROINIT:
      validate_span(instruction);
      check_pointer_value(instruction.first, index, environment);
      break;
    default:
      break;
  }
}

void validate_misc_instruction(const Instruction & instruction, const Function & function,
                               const SymbolIndex & index, const FunctionEnvironment & environment)
{
  switch(instruction.kind) {
    case Instruction::IK_CONST:
      if(instruction.type.text == "void") fail("void constant");
      if(instruction.first.literal_type.text != instruction.type.text) {
        fail("constant type mismatch");
      }
      break;
    case Instruction::IK_COPY:
      check_value_type(instruction.first, instruction.type, index, environment);
      break;
    case Instruction::IK_ADDR:
      if(instruction.first.kind == Operand::OP_GLOBAL) {
        if(index.symbols.find(instruction.first.text) == index.symbols.end()) {
          fail("address of undefined symbol");
        }
      } else {
        validate_operand_reference(instruction.first, index, environment);
      }
      break;
    case Instruction::IK_UNARY:
      validate_unary(instruction, index, environment);
      break;
    case Instruction::IK_BINARY:
      validate_binary(instruction, index, environment);
      break;
    case Instruction::IK_CMP:
      validate_compare(instruction, index, environment);
      break;
    case Instruction::IK_CONVERT:
      validate_conversion(instruction, index, environment);
      break;
    case Instruction::IK_CALL:
      validate_call(instruction, index, environment, !instruction.dest.empty());
      break;
    case Instruction::IK_EXCEPTION:
    case Instruction::IK_EXCEPTION_SELECTOR:
      if(instruction.type.text == "void") fail("exception value cannot be void");
      break;
    case Instruction::IK_STACK_ALLOC:
      if(instruction.byte_count == 0) fail("stack allocation size must be positive");
      break;
    case Instruction::IK_VA_START:
      check_pointer_value(instruction.first, index, environment);
      break;
    case Instruction::IK_VA_ARG:
      check_pointer_value(instruction.first, index, environment);
      break;
    case Instruction::IK_THROW:
      if(instruction.type.text == "void") fail("cannot throw void");
      check_value_type(instruction.first, instruction.type, index, environment);
      break;
    case Instruction::IK_BRANCH:
      check_value_type(instruction.first, make_type("i64"), index, environment);
      break;
    case Instruction::IK_SWITCH:
      check_value_type(instruction.first, make_type("i64"), index, environment);
      for(std::size_t i = 0; i < instruction.args.size(); i += 2) {
        check_value_type(instruction.args[i], make_type("i64"), index, environment);
      }
      break;
    case Instruction::IK_RETURN:
      if(function.return_type.text != instruction.type.text) fail("return type mismatch");
      if(is_type(instruction.type, "void")) {
        if(!instruction.first.text.empty()) fail("void return has a value");
      } else {
        if(instruction.first.text.empty()) fail("non-void return has no value");
        check_value_type(instruction.first, function.return_type, index, environment);
      }
      break;
    default:
      break;
  }
}

void ValidateInstructionTypes(const Instruction & instruction, const Function & function,
                             const SymbolIndex & index, const FunctionEnvironment & environment)
{
  validate_operand_references_for_instruction(instruction, index, environment);
  validate_memory_instruction(instruction, index, environment);
  validate_misc_instruction(instruction, function, index, environment);
}

void validate_operand_references_for_instruction(const Instruction & instruction,
                                                 const SymbolIndex & index,
                                                 const FunctionEnvironment & environment)
{
  const Operand * operands[] = { &instruction.first, &instruction.second, &instruction.third };
  for(std::size_t i = 0; i < sizeof(operands) / sizeof(operands[0]); ++i) {
    if(!operands[i]->text.empty() && operands[i]->kind != Operand::OP_LABEL) {
      validate_operand_reference(*operands[i], index, environment);
    }
  }
  for(std::size_t i = 0; i < instruction.args.size(); ++i) {
    if(instruction.args[i].kind != Operand::OP_LABEL) {
      validate_operand_reference(instruction.args[i], index, environment);
    }
  }
}

void ValidateFunctionBody(const Function & function, const SymbolIndex & index)
{
  if(function.blocks.empty()) fail("function has no blocks");
  FunctionEnvironment environment;
  collect_temporary_definitions(function, environment);
  for(std::size_t i = 0; i < function.blocks.size(); ++i) {
    const Block & block = function.blocks[i];
    if(block.instructions.empty()) fail("block has no terminator");
    bool terminated = false;
    for(std::size_t j = 0; j < block.instructions.size(); ++j) {
      const Instruction & instruction = block.instructions[j];
      if(terminated) fail("instruction appears after a terminator");
      ValidateInstructionTypes(instruction, function, index, environment);
      validate_control_flow(instruction, environment);
      validate_debug_location(instruction.debug_location);
      if(is_terminator(instruction.kind)) terminated = true;
    }
    if(!terminated) fail("block does not end in a terminator");
  }
}

bool is_eh_instruction(Instruction::Kind kind)
{
  return kind == Instruction::IK_EH_TRY || kind == Instruction::IK_EH_CLEANUP ||
         kind == Instruction::IK_EH_CLEANUP_CLAUSE || kind == Instruction::IK_EH_CATCH ||
         kind == Instruction::IK_EH_FILTER || kind == Instruction::IK_EH_CATCH_ALL ||
         kind == Instruction::IK_EH_END || kind == Instruction::IK_THROW ||
         kind == Instruction::IK_EXCEPTION || kind == Instruction::IK_EXCEPTION_SELECTOR ||
         kind == Instruction::IK_RESUME;
}

void collect_eh_fact(const Program & program, LowirProgramFacts & facts)
{
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    const std::vector< Block > & blocks = program.functions[i].blocks;
    for(std::size_t j = 0; j < blocks.size(); ++j) {
      for(std::size_t k = 0; k < blocks[j].instructions.size(); ++k) {
        if(is_eh_instruction(blocks[j].instructions[k].kind)) {
          facts.uses_eh = true;
          return;
        }
      }
    }
  }
}

}  // namespace

LowirProgramFacts ValidateLowirProgram(const lowir_model::Program & program)
{
  LowirProgramFacts facts;
  facts.global_declaration_count = program.global_declarations.size();
  facts.global_definition_count = program.globals.size();
  facts.function_declaration_count = program.function_declarations.size();
  facts.function_definition_count = program.functions.size();
  facts.alias_count = program.object_aliases.size();

  SymbolIndex index;
  ValidateTopLevelSymbols(program, index, facts);
  ValidateMetadata(program);
  ValidateGlobals(program, index);
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    ValidateFunctionBody(program.functions[i], index);
  }
  collect_eh_fact(program, facts);
  return facts;
}
