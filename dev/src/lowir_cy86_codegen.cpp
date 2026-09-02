#include "lowir_cy86_codegen.h"

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using lowir_model::Function;
using lowir_model::FunctionDeclaration;
using lowir_model::Instruction;
using lowir_model::LowType;
using lowir_model::LowTypeInfo;
using lowir_model::Operand;
using lowir_model::Parameter;
using lowir_model::Program;

bool is_wide(const LowType & type)
{
  const LowTypeInfo info = lowir_model::describe_low_type(type);
  return (info.floating() && type.text == "f80") || info.object();
}

std::size_t value_size(const LowType & type)
{
  const LowTypeInfo info = lowir_model::describe_low_type(type);
  if(type.text == "f80") return 16;
  if(info.object()) return (info.bytes + 7) & ~static_cast<std::size_t>(7);
  return 8;
}

std::string number(std::size_t value)
{
  std::ostringstream out;
  out << value;
  return out.str();
}

std::string signed_number(long long value)
{
  std::ostringstream out;
  out << value;
  return out.str();
}

std::string strip_sigil(const std::string & name)
{
  return name.size() > 1 ? name.substr(1) : name;
}

std::string function_label(const std::string & name)
{
  return "fn__" + strip_sigil(name);
}

std::string global_label(const std::string & name)
{
  return "g__" + strip_sigil(name);
}

std::string memory(const std::string & base, long long offset)
{
  if(offset == 0) return "[" + base + "]";
  if(offset > 0) return "[" + base + "+" + signed_number(offset) + "]";
  return "[" + base + signed_number(offset) + "]";
}

std::string local_memory(long long offset)
{
  return memory("bp", offset);
}

std::string width_suffix(const LowType & type)
{
  const LowTypeInfo info = lowir_model::describe_low_type(type);
  if(type.text == "f32") return "32";
  if(type.text == "f64") return "64";
  if(info.pointer()) return "64";
  if(info.integer()) {
    if(info.bits <= 8) return "8";
    if(info.bits <= 16) return "16";
    if(info.bits <= 32) return "32";
    return "64";
  }
  return "64";
}

std::string literal_text(const Operand & operand)
{
  return operand.text == "nullptr" ? "0" : operand.text;
}

LowType result_type(const Instruction & instruction)
{
  if(instruction.kind == Instruction::IK_ADDR ||
     instruction.kind == Instruction::IK_INDEX ||
     instruction.kind == Instruction::IK_STACK_ALLOC ||
     instruction.kind == Instruction::IK_VA_START) {
    LowType type;
    type.text = "ptr";
    return type;
  }
  if(instruction.kind == Instruction::IK_CMP) {
    LowType type;
    type.text = "i64";
    return type;
  }
  return instruction.type;
}

class Cy86Writer
{
public:
  void instruction(const std::string & op, const std::string & operands)
  {
    out_ << "\t" << op;
    if(!operands.empty()) out_ << " " << operands;
    out_ << ";\n";
  }

  void label(const std::string & name)
  {
    out_ << name << ":\n";
  }

  void blank()
  {
    out_ << "\n";
  }

  std::string str() const
  {
    return out_.str();
  }

private:
  std::ostringstream out_;
};

struct ValueInfo
{
  long long offset = 0;
  LowType type;
};

struct AbiParameter
{
  std::string name;
  LowType type;
};

struct FrameLayout
{
  std::unordered_map<std::string, ValueInfo> values;
  std::vector<AbiParameter> abi_params;
  std::size_t local_size = 0;
  std::size_t frame_size = 0;
  bool has_scratch = false;
  std::string hidden_result;
};

bool type_is_f80(const LowType & type)
{
  return type.text == "f80";
}

bool instruction_needs_scratch(const Instruction & instruction)
{
  if(instruction.kind == Instruction::IK_CONVERT) return true;
  if(type_is_f80(instruction.type) || type_is_f80(instruction.source_type) ||
     type_is_f80(instruction.call_return_type)) return true;
  for(std::size_t i = 0; i < instruction.call_params.size(); ++i) {
    if(type_is_f80(instruction.call_params[i].type)) return true;
  }
  return false;
}

void add_value(FrameLayout & layout, const std::string & name,
               const LowType & type, std::size_t & used)
{
  const std::size_t size = value_size(type);
  used += size;
  ValueInfo value;
  value.offset = -static_cast<long long>(used);
  value.type = type;
  layout.values[name] = value;
}

FrameLayout build_frame(const Function & function)
{
  FrameLayout layout;
  std::size_t used = 0;
  if(is_wide(function.return_type)) {
    layout.hidden_result = "%__cppgm_hidden_result";
    LowType pointer;
    pointer.text = "ptr";
    layout.abi_params.push_back(AbiParameter{layout.hidden_result, pointer});
    add_value(layout, layout.hidden_result, pointer, used);
  }
  for(std::size_t i = 0; i < function.params.size(); ++i) {
    layout.abi_params.push_back(AbiParameter{function.params[i].name,
                                              function.params[i].type});
    add_value(layout, function.params[i].name, function.params[i].type, used);
    if(is_wide(function.params[i].type)) layout.has_scratch = true;
  }
  for(std::size_t i = 0; i < function.slots.size(); ++i) {
    add_value(layout, function.slots[i].first, function.slots[i].second, used);
  }
  for(std::size_t i = 0; i < function.blocks.size(); ++i) {
    for(std::size_t j = 0; j < function.blocks[i].instructions.size(); ++j) {
      const Instruction & instruction = function.blocks[i].instructions[j];
      if(!instruction.dest.empty() &&
         layout.values.find(instruction.dest) == layout.values.end()) {
        add_value(layout, instruction.dest, result_type(instruction), used);
      }
      if(instruction_needs_scratch(instruction)) layout.has_scratch = true;
    }
  }
  layout.local_size = used;
  layout.frame_size = used + (layout.has_scratch ? 64 : 0);
  return layout;
}

class CodegenContext
{
public:
  CodegenContext(const Program & program, const LowirProgramFacts & facts)
    : program_(program), facts_(facts)
  {}

  const Function * definition(const std::string & name) const
  {
    const std::unordered_map<std::string, LowirProgramFacts::SymbolRef>::const_iterator found =
      facts_.symbols.find(name);
    if(found == facts_.symbols.end() ||
       found->second.kind != LowirProgramFacts::SymbolRef::DEF_FUNCTION) return 0;
    return &program_.functions[found->second.index];
  }

  const FunctionDeclaration * declaration(const std::string & name) const
  {
    const std::unordered_map<std::string, LowirProgramFacts::SymbolRef>::const_iterator found =
      facts_.symbols.find(name);
    if(found == facts_.symbols.end() ||
       found->second.kind != LowirProgramFacts::SymbolRef::DECL_FUNCTION) return 0;
    return &program_.function_declarations[found->second.index];
  }

  bool is_function(const std::string & name) const
  {
    const std::unordered_map<std::string, LowirProgramFacts::SymbolRef>::const_iterator found =
      facts_.symbols.find(name);
    if(found == facts_.symbols.end()) return false;
    return found->second.kind == LowirProgramFacts::SymbolRef::DECL_FUNCTION ||
           found->second.kind == LowirProgramFacts::SymbolRef::DEF_FUNCTION;
  }

  std::string next_label(const std::string & prefix)
  {
    return prefix + number(label_counter_++);
  }

  const Program & program() const
  {
    return program_;
  }

private:
  const Program & program_;
  const LowirProgramFacts & facts_;
  std::size_t label_counter_ = 0;
};

}  // namespace

namespace {

class FunctionEmitter
{
public:
  FunctionEmitter(const Function & function, CodegenContext & context,
                  Cy86Writer & writer)
    : function_(function), context_(context), writer_(writer),
      layout_(build_frame(function))
  {}

  void emit()
  {
    emit_prologue();
    for(std::size_t i = 0; i < function_.blocks.size(); ++i) {
      writer_.label(block_label(function_.blocks[i].label));
      for(std::size_t j = 0; j < function_.blocks[i].instructions.size(); ++j) {
        emit_instruction(function_.blocks[i].instructions[j]);
      }
    }
    emit_epilogue();
  }

private:
  const Function & function_;
  CodegenContext & context_;
  Cy86Writer & writer_;
  FrameLayout layout_;

  const ValueInfo & value(const std::string & name) const
  {
    const std::unordered_map<std::string, ValueInfo>::const_iterator found =
      layout_.values.find(name);
    if(found == layout_.values.end()) {
      throw std::runtime_error("missing LowIR value layout for " + name);
    }
    return found->second;
  }

  std::string block_label(const std::string & name) const
  {
    return function_label(function_.name) + "__" + strip_sigil(name);
  }

  std::string epilogue_label() const
  {
    return function_label(function_.name) + "__epilogue";
  }

  std::string parameter_register(std::size_t index) const
  {
    static const char * const names[] = { "x", "y", "z", "t" };
    return index < 4 ? names[index] : "x";
  }

  void emit_prologue()
  {
    writer_.label(function_label(function_.name));
    writer_.instruction("isub64", "sp sp 8");
    writer_.instruction("move64", "[sp] bp");
    writer_.instruction("move64", "bp sp");
    if(layout_.frame_size != 0) {
      writer_.instruction("isub64", "sp sp " + number(layout_.frame_size));
    }
    for(std::size_t i = 0; i < layout_.abi_params.size(); ++i) {
      emit_parameter_spill(i, layout_.abi_params[i]);
    }
  }

  void emit_parameter_spill(std::size_t index, const AbiParameter & parameter)
  {
    if(is_wide(parameter.type)) {
      emit_wide_parameter_spill(index, parameter);
      return;
    }
    const ValueInfo & target = value(parameter.name);
    if(index < 4) {
      writer_.instruction("move64", local_memory(target.offset) + " " +
                          parameter_register(index) + "64");
    } else {
      const long long stack_offset = 16 + static_cast<long long>((index - 4) * 8);
      writer_.instruction("move64", "x64 " + memory("bp", stack_offset));
      writer_.instruction("move64", local_memory(target.offset) + " x64");
    }
  }

  void emit_wide_parameter_spill(std::size_t index,
                                 const AbiParameter & parameter)
  {
    const ValueInfo & target = value(parameter.name);
    if(index < 4) {
      writer_.instruction("move64", "x64 " + parameter_register(index) + "64");
    } else {
      const long long stack_offset = 16 + static_cast<long long>((index - 4) * 8);
      writer_.instruction("move64", "x64 " + memory("bp", stack_offset));
    }
    const std::size_t size = value_size(parameter.type);
    for(std::size_t offset = 0; offset < size; offset += 8) {
      writer_.instruction("move64", register_memory("z", offset) + " " +
                          memory("x64", static_cast<long long>(offset)));
      writer_.instruction("move64", local_memory(target.offset +
                          static_cast<long long>(offset)) + " z64");
    }
  }

  static std::string register_memory(const std::string & base,
                                     std::size_t offset)
  {
    return memory(base + "64", static_cast<long long>(offset));
  }

  void emit_epilogue()
  {
    writer_.label(epilogue_label());
    writer_.instruction("move64", "sp bp");
    writer_.instruction("move64", "bp [sp]");
    writer_.instruction("iadd64", "sp sp 8");
    writer_.instruction("ret", "");
  }

  std::string operand_type_name(const Operand & operand,
                                const std::string & fallback) const
  {
    if(operand.kind == Operand::OP_TEMP || operand.kind == Operand::OP_SLOT) {
      return value(operand.text).type.text;
    }
    if(operand.kind == Operand::OP_GLOBAL && context_.is_function(operand.text)) {
      return "ptr";
    }
    if(!operand.literal_type.text.empty()) return operand.literal_type.text;
    return fallback;
  }

  void load_value(const Operand & operand, const LowType & type,
                  const std::string & base)
  {
    if(operand.kind == Operand::OP_TEMP) {
      const ValueInfo & source = value(operand.text);
      if(is_wide(source.type)) {
        writer_.instruction("isub64", base + "64 bp " +
                            number(static_cast<std::size_t>(-source.offset)));
      } else {
        const LowType & effective = type.text.empty() ? source.type : type;
        const std::string suffix = width_suffix(effective);
        if(suffix == "8" || suffix == "16") {
          writer_.instruction("move64", base + "64 0");
        }
        writer_.instruction("move" + suffix,
                            base + suffix + " " + local_memory(source.offset));
      }
      return;
    }
    if(operand.kind == Operand::OP_SLOT) {
      const ValueInfo & source = value(operand.text);
      writer_.instruction("isub64", base + "64 bp " +
                          number(static_cast<std::size_t>(-source.offset)));
      return;
    }
    if(operand.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move64", base + "64 " +
                          (context_.is_function(operand.text) ?
                           function_label(operand.text) : global_label(operand.text)));
      return;
    }
    const std::string suffix = type.text == "f32" ? "32" : "64";
    writer_.instruction("move" + suffix, base + suffix + " " +
                        literal_text(operand));
  }

  void load_address(const Operand & operand, const std::string & base)
  {
    if(operand.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move64", base + "64 " +
                          (context_.is_function(operand.text) ?
                           function_label(operand.text) : global_label(operand.text)));
    } else if(operand.kind == Operand::OP_TEMP || operand.kind == Operand::OP_SLOT) {
      const ValueInfo & source = value(operand.text);
      writer_.instruction("isub64", base + "64 bp " +
                          number(static_cast<std::size_t>(-source.offset)));
    } else {
      load_value(operand, LowType(), base);
    }
  }

  void store_result(const std::string & destination, const LowType & type,
                    const std::string & base)
  {
    const ValueInfo & target = value(destination);
    if(is_wide(target.type)) {
      throw std::runtime_error("wide result is outside the scalar CY86 checkpoint");
    }
    const std::string suffix = width_suffix(type);
    writer_.instruction("move" + suffix,
                        local_memory(target.offset) + " " + base + suffix);
  }

  void emit_const(const Instruction & instruction)
  {
    load_value(instruction.first, instruction.type, "x");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_copy(const Instruction & instruction)
  {
    load_value(instruction.first, instruction.type, "x");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_addr(const Instruction & instruction)
  {
    load_address(instruction.first, "x");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_load(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    if(instruction.first.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move" + suffix,
                          "x" + suffix + " [" + global_label(instruction.first.text) + "]");
    } else if(instruction.first.kind == Operand::OP_SLOT) {
      writer_.instruction("move" + suffix,
                          "x" + suffix + " " + local_memory(value(instruction.first.text).offset));
    } else {
      load_value(instruction.first, LowType(), "x");
      writer_.instruction("move" + suffix,
                          "x" + suffix + " [x64]");
    }
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_store(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    load_value(instruction.first, instruction.type, "x");
    if(instruction.second.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move" + suffix,
                          "[" + global_label(instruction.second.text) + "] x" + suffix);
    } else if(instruction.second.kind == Operand::OP_SLOT) {
      writer_.instruction("move" + suffix,
                          local_memory(value(instruction.second.text).offset) + " x" + suffix);
    } else {
      load_value(instruction.second, LowType(), "y");
      writer_.instruction("move" + suffix,
                          "[y64] x" + suffix);
    }
  }

  void emit_binary(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    load_value(instruction.first, instruction.type, "y");
    load_value(instruction.second, instruction.type, "x");
    if(instruction.op == "shl" || instruction.op == "shr" ||
       instruction.op == "ushr") {
      writer_.instruction("move64", "z64 x64");
      writer_.instruction("move8", "x8 z8");
      writer_.instruction((instruction.op == "shl" ? "lshift" :
                           instruction.op == "shr" ? "srshift" : "urshift") +
                          suffix, "x" + suffix + " y" + suffix + " x8");
    } else {
      writer_.instruction((info.floating() ? "f" + instruction.op :
                           binary_opcode(instruction.op)) + suffix,
                          "x" + suffix + " y" + suffix + " x" + suffix);
    }
    store_result(instruction.dest, instruction.type, "x");
  }

  std::string binary_opcode(const std::string & op) const
  {
    if(op == "add") return "iadd";
    if(op == "sub") return "isub";
    if(op == "mul") return "smul";
    if(op == "div") return "sdiv";
    if(op == "mod") return "smod";
    if(op == "udiv") return "udiv";
    if(op == "umod") return "umod";
    if(op == "and") return "and";
    if(op == "or") return "or";
    if(op == "xor") return "xor";
    return "f" + op;
  }

  void emit_compare(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    load_value(instruction.first, instruction.type, "y");
    load_value(instruction.second, instruction.type, "x");
    writer_.instruction((info.floating() ? "f" + instruction.op :
                         compare_opcode(instruction.op)) + suffix,
                        "z8 y" + suffix + " x" + suffix);
    writer_.instruction("move64", "x64 0");
    writer_.instruction("move8", "x8 z8");
    LowType result;
    result.text = "i64";
    store_result(instruction.dest, result, "x");
  }

  std::string compare_opcode(const std::string & op) const
  {
    if(op == "eq") return "ieq";
    if(op == "ne") return "ine";
    if(op == "lt") return "slt";
    if(op == "le") return "sle";
    if(op == "gt") return "sgt";
    if(op == "ge") return "sge";
    if(op == "ult") return "ult";
    if(op == "ule") return "ule";
    if(op == "ugt") return "ugt";
    if(op == "uge") return "uge";
    return "feq";
  }

  void emit_unary(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    if(instruction.op == "decay") {
      load_value(instruction.first, instruction.type, "x");
    } else if(instruction.op == "neg") {
      load_value(instruction.first, instruction.type, "x");
      writer_.instruction("move" + suffix, "y" + suffix + " 0");
      writer_.instruction("isub" + suffix,
                          "x" + suffix + " y" + suffix + " x" + suffix);
    } else if(instruction.op == "not") {
      load_value(instruction.first, instruction.type, "x");
      writer_.instruction("ieq64", "z8 x64 0");
      writer_.instruction("move64", "x64 0");
      writer_.instruction("move8", "x8 z8");
    } else if(instruction.op == "bitnot") {
      load_value(instruction.first, instruction.type, "x");
      writer_.instruction("not" + suffix,
                          "x" + suffix + " x" + suffix);
    } else if(instruction.op == "bswap") {
      load_value(instruction.first, instruction.type, "x");
      writer_.instruction("bswap" + suffix,
                          "x" + suffix + " x" + suffix);
    } else {
      throw std::runtime_error("unsupported unary operation");
    }
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_convert(const Instruction & instruction)
  {
    if(instruction.op != "zext" && instruction.op != "sext" &&
       instruction.op != "trunc") {
      throw std::runtime_error("wide conversion is outside the scalar CY86 checkpoint");
    }
    load_value(instruction.first, instruction.source_type, "x");
    if(instruction.op == "sext") {
      const LowTypeInfo source = lowir_model::describe_low_type(instruction.source_type);
      writer_.instruction("move8", "t8 " + number(64 - source.bits));
      writer_.instruction("lshift64", "x64 x64 t8");
      writer_.instruction("srshift64", "x64 x64 t8");
    }
    store_result(instruction.dest, instruction.type, "x");
  }

  std::size_t index_element_size(const LowType & type) const
  {
    const LowTypeInfo info = lowir_model::describe_low_type(type);
    if(info.object()) return info.bytes;
    if(type.text == "f80") return 16;
    return info.bytes;
  }

  void emit_index(const Instruction & instruction)
  {
    load_value(instruction.first, LowType(), "y");
    LowType index_type;
    index_type.text = "i64";
    load_value(instruction.second, index_type, "x");
    const std::size_t element_size = index_element_size(instruction.type);
    if(element_size != 1) {
      writer_.instruction("move64", "z64 " + number(element_size));
      writer_.instruction("smul64", "x64 x64 z64");
    }
    writer_.instruction("iadd64", "x64 y64 x64");
    LowType pointer;
    pointer.text = "ptr";
    store_result(instruction.dest, pointer, "x");
  }

  void load_bulk_pointer(const Operand & operand, const std::string & base)
  {
    if((operand.kind == Operand::OP_TEMP || operand.kind == Operand::OP_SLOT) &&
       is_wide(value(operand.text).type)) {
      load_address(operand, base);
    } else {
      load_value(operand, LowType(), base);
    }
  }

  void emit_copyobj(const Instruction & instruction)
  {
    load_bulk_pointer(instruction.second, "x");
    load_bulk_pointer(instruction.first, "y");
    for(std::size_t offset = 0; offset < instruction.byte_count; offset += 8) {
      writer_.instruction("move64", "z64 " + memory("y64", 0));
      writer_.instruction("move64", memory("x64", 0) + " z64");
      if(offset + 8 < instruction.byte_count) {
        writer_.instruction("iadd64", "x64 x64 8");
        writer_.instruction("iadd64", "y64 y64 8");
      }
    }
  }

  void emit_zeroinit(const Instruction & instruction)
  {
    load_bulk_pointer(instruction.first, "x");
    writer_.instruction("move64", "z64 0");
    for(std::size_t offset = 0; offset < instruction.byte_count; offset += 8) {
      writer_.instruction("move64", memory("x64", 0) + " z64");
      if(offset + 8 < instruction.byte_count) {
        writer_.instruction("iadd64", "x64 x64 8");
      }
    }
  }

  const std::vector<Parameter> * call_parameters(const Instruction & instruction) const
  {
    if(instruction.has_call_signature) return &instruction.call_params;
    const Function * definition = context_.definition(instruction.first.text);
    if(definition != 0) return &definition->params;
    const FunctionDeclaration * declaration = context_.declaration(instruction.first.text);
    if(declaration != 0) return &declaration->params;
    return 0;
  }

  LowType call_parameter_type(const Instruction & instruction,
                              std::size_t index) const
  {
    const std::vector<Parameter> * parameters = call_parameters(instruction);
    if(parameters != 0 && index < parameters->size()) {
      return (*parameters)[index].type;
    }
    if(index < instruction.args.size()) {
      LowType type;
      type.text = operand_type_name(instruction.args[index], "i64");
      return type;
    }
    LowType type;
    type.text = "i64";
    return type;
  }

  bool call_argument_is_address(const Instruction & instruction,
                                std::size_t index) const
  {
    const LowType expected = call_parameter_type(instruction, index);
    if(is_wide(expected)) return true;
    if(index >= instruction.args.size()) return false;
    const Operand & operand = instruction.args[index];
    return operand.kind == Operand::OP_SLOT ||
           ((operand.kind == Operand::OP_TEMP) && is_wide(value(operand.text).type));
  }

  void emit_call_argument(const Instruction & instruction, std::size_t index,
                          const std::string & target)
  {
    const LowType expected = call_parameter_type(instruction, index);
    const Operand & operand = instruction.args[index];
    if(call_argument_is_address(instruction, index)) {
      load_address(operand, "x");
      writer_.instruction("move64", target + "64 x64");
    } else {
      load_value(operand, expected, target);
    }
  }

  void emit_call(const Instruction & instruction)
  {
    const bool indirect = instruction.first.kind != Operand::OP_GLOBAL;
    const std::size_t argument_count = instruction.args.size();
    if(indirect) {
      load_value(instruction.first, LowType(), "x");
      writer_.instruction("isub64", "sp sp 8");
      writer_.instruction("move64", "[sp] x64");
    } else if(argument_count > 4) {
      writer_.instruction("isub64", "sp sp " +
                          number((argument_count - 4) * 8));
    }
    for(std::size_t i = 0; i < argument_count; ++i) {
      if(i < 4) {
        static const char * const registers[] = { "x", "y", "z", "t" };
        emit_call_argument(instruction, i, registers[i]);
      } else {
        emit_call_argument(instruction, i, "x");
        const long long offset = static_cast<long long>((i - 4) * 8);
        writer_.instruction("move64", memory("sp", offset) + " 0");
        writer_.instruction("move64", memory("sp", offset) + " x64");
      }
    }
    if(indirect) {
      writer_.instruction("call", "[sp]");
      writer_.instruction("iadd64", "sp sp 8");
    } else {
      writer_.instruction("call", function_label(instruction.first.text));
      if(argument_count > 4) {
        writer_.instruction("iadd64", "sp sp " +
                            number((argument_count - 4) * 8));
      }
    }
    if(!instruction.dest.empty()) {
      store_result(instruction.dest, instruction.type, "x");
    }
  }

  void emit_atomic_load(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    load_value(instruction.first, LowType(), "y");
    writer_.instruction("move" + suffix,
                        "x" + suffix + " [y64]");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_atomic_store(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    load_value(instruction.second, LowType(), "y");
    load_value(instruction.first, instruction.type, "x");
    writer_.instruction("move" + suffix,
                        "[y64] x" + suffix);
  }

  void emit_atomic_exchange(const Instruction & instruction)
  {
    load_value(instruction.first, LowType(), "y");
    load_value(instruction.second, instruction.type, "x");
    writer_.instruction("move64", "t64 [y64]");
    writer_.instruction("move64", "[y64] x64");
    writer_.instruction("move64", "x64 0");
    writer_.instruction("move64", "x64 t64");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_atomic_add_fetch(const Instruction & instruction)
  {
    load_value(instruction.first, LowType(), "y");
    writer_.instruction("move64", "x64 [y64]");
    load_value(instruction.second, instruction.type, "z");
    writer_.instruction("iadd64", "x64 x64 z64");
    writer_.instruction("move64", "[y64] x64");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_atomic_compare_exchange(const Instruction & instruction)
  {
    load_value(instruction.first, LowType(), "y");
    load_value(instruction.second, LowType(), "z");
    writer_.instruction("move64", "t64 [y64]");
    writer_.instruction("move64", "x64 [z64]");
    writer_.instruction("ieq64", "x8 t64 x64");
    const std::string success = context_.next_label("__atomic_cmpxchg_success__");
    const std::string end = context_.next_label("__atomic_cmpxchg_end__");
    writer_.instruction("jumpif", "x8 " + success);
    writer_.instruction("move64", "[z64] t64");
    writer_.instruction("move64", "x64 0");
    store_result(instruction.dest, instruction.type, "x");
    writer_.instruction("jump", end);
    writer_.label(success);
    load_value(instruction.third, instruction.type, "x");
    writer_.instruction("move64", "[y64] x64");
    writer_.instruction("move64", "x64 1");
    store_result(instruction.dest, instruction.type, "x");
    writer_.label(end);
  }

  void emit_jump(const Instruction & instruction)
  {
    writer_.instruction("jump", block_label(instruction.first.text));
  }

  void emit_branch(const Instruction & instruction)
  {
    LowType condition;
    condition.text = "i64";
    load_value(instruction.first, condition, "x");
    writer_.instruction("ieq64", "z8 x64 0");
    writer_.instruction("jumpif", "z8 " + block_label(instruction.third.text));
    writer_.instruction("jump", block_label(instruction.second.text));
  }

  void emit_switch(const Instruction & instruction)
  {
    LowType selector;
    selector.text = "i64";
    load_value(instruction.first, selector, "x");
    for(std::size_t i = 0; i < instruction.args.size(); i += 2) {
      load_value(instruction.args[i], selector, "t");
      writer_.instruction("ieq64", "z8 x64 t64");
      writer_.instruction("jumpif", "z8 " +
                          block_label(instruction.args[i + 1].text));
    }
    writer_.instruction("jump", block_label(instruction.second.text));
  }

  void emit_return(const Instruction & instruction)
  {
    if(instruction.type.text != "void") {
      load_value(instruction.first, instruction.type, "x");
    }
    writer_.instruction("jump", epilogue_label());
  }

  void emit_instruction(const Instruction & instruction)
  {
    switch(instruction.kind) {
      case Instruction::IK_CONST: emit_const(instruction); break;
      case Instruction::IK_COPY: emit_copy(instruction); break;
      case Instruction::IK_ADDR: emit_addr(instruction); break;
      case Instruction::IK_LOAD: emit_load(instruction); break;
      case Instruction::IK_STORE: emit_store(instruction); break;
      case Instruction::IK_ATOMIC_LOAD: emit_atomic_load(instruction); break;
      case Instruction::IK_ATOMIC_STORE: emit_atomic_store(instruction); break;
      case Instruction::IK_ATOMIC_EXCHANGE: emit_atomic_exchange(instruction); break;
      case Instruction::IK_ATOMIC_ADD_FETCH: emit_atomic_add_fetch(instruction); break;
      case Instruction::IK_ATOMIC_COMPARE_EXCHANGE:
        emit_atomic_compare_exchange(instruction);
        break;
      case Instruction::IK_ATOMIC_THREAD_FENCE:
      case Instruction::IK_ATOMIC_SIGNAL_FENCE:
        break;
      case Instruction::IK_INDEX: emit_index(instruction); break;
      case Instruction::IK_UNARY: emit_unary(instruction); break;
      case Instruction::IK_BINARY: emit_binary(instruction); break;
      case Instruction::IK_CMP: emit_compare(instruction); break;
      case Instruction::IK_CONVERT: emit_convert(instruction); break;
      case Instruction::IK_COPYOBJ: emit_copyobj(instruction); break;
      case Instruction::IK_ZEROINIT: emit_zeroinit(instruction); break;
      case Instruction::IK_CALL: emit_call(instruction); break;
      case Instruction::IK_JUMP: emit_jump(instruction); break;
      case Instruction::IK_BRANCH: emit_branch(instruction); break;
      case Instruction::IK_SWITCH: emit_switch(instruction); break;
      case Instruction::IK_RETURN: emit_return(instruction); break;
      default:
        throw std::runtime_error("unsupported LowIR instruction in scalar CY86 checkpoint");
    }
  }
};

void emit_start(const Program & program, const LowirProgramFacts & facts,
                Cy86Writer & writer)
{
  writer.label("start");
  writer.instruction("move64", "bp sp");
  if(facts.init >= 0) {
    writer.instruction("call", function_label(program.functions[facts.init].name));
  }
  writer.instruction("call", function_label(program.functions[facts.entry].name));
  if(facts.fini >= 0) {
    writer.instruction("isub64", "sp sp 8");
    writer.instruction("move64", "[sp] x64");
    writer.instruction("call", function_label(program.functions[facts.fini].name));
    writer.instruction("move64", "x64 [sp]");
    writer.instruction("iadd64", "sp sp 8");
  }
  writer.instruction("syscall1", "t64 60 x64");
}

void emit_global_scalar(const lowir_model::GlobalDefinition & global,
                        const CodegenContext & context, Cy86Writer & writer)
{
  writer.label(global_label(global.name));
  if(global.init_kind == lowir_model::GlobalDefinition::INIT_ZERO) {
    const std::string suffix = width_suffix(global.type);
    writer.instruction("data" + suffix, "0");
  } else if(global.init_kind == lowir_model::GlobalDefinition::INIT_ADDR) {
    const std::string target = context.is_function(global.init_operand.text) ?
      function_label(global.init_operand.text) : global_label(global.init_operand.text);
    writer.instruction("data64", target +
                       (global.addr_addend == 0 ? std::string() :
                        (global.addr_addend > 0 ? "+" : "") +
                        signed_number(global.addr_addend)));
  } else {
    writer.instruction("data" + width_suffix(global.type),
                       literal_text(global.init_operand));
  }
}

void emit_global_structured(const lowir_model::GlobalDefinition & global,
                            const CodegenContext & context, Cy86Writer & writer)
{
  std::size_t offset = 0;
  writer.label(global_label(global.name));
  for(std::size_t i = 0; i < global.data_items.size(); ++i) {
    const lowir_model::GlobalDefinition::DataItem & item = global.data_items[i];
    if(item.kind == lowir_model::GlobalDefinition::DataItem::ITEM_ZERO) {
      for(std::size_t j = 0; j < item.zero_bytes; ++j) {
        writer.instruction("data8", "0");
      }
      offset += item.zero_bytes;
      continue;
    }
    const LowTypeInfo info = lowir_model::describe_low_type(item.type);
    const std::size_t alignment = info.alignment;
    const std::size_t padding = alignment == 0 ? 0 :
      (alignment - (offset % alignment)) % alignment;
    for(std::size_t j = 0; j < padding; ++j) {
      writer.instruction("data8", "0");
    }
    offset += padding;
    if(item.kind == lowir_model::GlobalDefinition::DataItem::ITEM_ADDR) {
      const std::string target = context.is_function(item.symbol) ?
        function_label(item.symbol) : global_label(item.symbol);
      writer.instruction("data64", target +
                         (item.addr_addend == 0 ? std::string() :
                          (item.addr_addend > 0 ? "+" : "") +
                          signed_number(item.addr_addend)));
    } else {
      writer.instruction("data" + width_suffix(item.type),
                         literal_text(item.literal_operand));
    }
    offset += info.bytes;
  }
}

void emit_globals(const Program & program, const CodegenContext & context,
                  Cy86Writer & writer)
{
  for(std::size_t i = 0; i < program.globals.size(); ++i) {
    writer.blank();
    if(program.globals[i].structured) {
      emit_global_structured(program.globals[i], context, writer);
    } else {
      emit_global_scalar(program.globals[i], context, writer);
    }
  }
}

}  // namespace

std::string EmitCy86Program(const lowir_model::Program & program,
                            const LowirProgramFacts & facts)
{
  CodegenContext context(program, facts);
  Cy86Writer writer;
  emit_start(program, facts, writer);
  for(std::size_t i = 0; i < program.functions.size(); ++i) {
    writer.blank();
    FunctionEmitter(program.functions[i], context, writer).emit();
  }
  emit_globals(program, context, writer);
  return writer.str();
}
