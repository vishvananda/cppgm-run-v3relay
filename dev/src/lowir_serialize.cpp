#include "lowir_model.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace lowir_model {

namespace {

std::string operand(const Operand& value)
{
  if (value.kind == Operand::OP_INTEGER && value.text == "nullptr")
    return "nullptr";
  if (value.kind == Operand::OP_INTEGER)
    return std::to_string(value.int_value);
  if (value.kind == Operand::OP_FLOAT) {
    if (!value.text.empty())
      return value.text;
    std::ostringstream out;
    out << std::setprecision(20) << value.float_value;
    return out.str();
  }
  return value.text;
}

const char* role_name(SymbolRole role)
{
  static const char* const names[] = {
    "none", "entry", "init", "fini", "eh_top", "eh_value", "eh_type",
    "eh_unhandled", "eh_allocate_exception", "eh_begin_catch",
    "eh_call_unexpected", "eh_current_exception_type", "eh_end_catch",
    "eh_rethrow", "eh_throw", "eh_personality", "eh_resume"
  };
  return names[static_cast<unsigned>(role)];
}

// Metadata items shared by symbol declarations, definitions and call
// boundaries; each writer appends to one bracketed list.
void append_symbol_items(std::ostringstream& body, bool& first,
                         const SymbolMetadata& metadata,
                         GlobalStorageMode storage)
{
  if (storage == GSM_THREAD_LOCAL) {
    body << (first ? "" : ", ") << "storage=thread_local";
    first = false;
  }
  if (metadata.role != SR_NONE) { body << (first ? "" : ", ") << "role=" << role_name(metadata.role); first = false; }
  if (metadata.linkage != LLM_DEFAULT) { body << (first ? "" : ", ") << "linkage=" << (metadata.linkage == LLM_C ? "c" : "cpp"); first = false; }
  if (metadata.binding != SBM_DEFAULT) {
    const char* value = metadata.binding == SBM_INTERNAL ? "internal" :
        metadata.binding == SBM_STRONG ? "strong" : "weak";
    body << (first ? "" : ", ") << "binding=" << value; first = false;
  }
  if (!metadata.object_symbol.empty()) { body << (first ? "" : ", ") << "object=" << metadata.object_symbol; first = false; }
  if (!metadata.tls_for_symbol.empty()) { body << (first ? "" : ", ") << "tls_for=" << metadata.tls_for_symbol; first = false; }
  if (!metadata.section_segment.empty()) { body << (first ? "" : ", ") << "section_segment=" << metadata.section_segment; first = false; }
  if (!metadata.section_name.empty()) { body << (first ? "" : ", ") << "section=" << metadata.section_name; first = false; }
  if (metadata.keep_internal_alias) { body << (first ? "" : ", ") << "keep_alias=yes"; first = false; }
  if (metadata.prefer_local_object_binding) { body << (first ? "" : ", ") << "prefer_local=yes"; first = false; }
  if (metadata.object_output_root) { body << (first ? "" : ", ") << "object_root=yes"; first = false; }
  if (metadata.object_trivial_lifecycle) { body << (first ? "" : ", ") << "trivial_lifecycle=yes"; first = false; }
  if (metadata.force_inline) { body << (first ? "" : ", ") << "force_inline=yes"; first = false; }
}

void append_boundary_items(std::ostringstream& body, bool& first,
                           const FunctionBoundaryMetadata& boundary)
{
  if (boundary.arity != CAM_FIXED) { body << (first ? "" : ", ") << "arity=" << (boundary.arity == CAM_VARIADIC ? "variadic" : "prototype_relaxed"); first = false; }
  if (boundary.effects != CFXM_DEFAULT) { body << (first ? "" : ", ") << "effects=" << (boundary.effects == CFXM_READNONE ? "readnone" : boundary.effects == CFXM_READONLY ? "readonly" : "readwrite"); first = false; }
  if (boundary.unwind != CUM_DEFAULT) { body << (first ? "" : ", ") << "unwind=" << (boundary.unwind == CUM_MAY ? "may" : "no"); first = false; }
  if (boundary.returns != CRM_DEFAULT) { body << (first ? "" : ", ") << "return=" << (boundary.returns == CRM_RETURNS ? "returns" : "noreturn"); first = false; }
}

void append_symbol_metadata(std::ostringstream& out,
                            const SymbolMetadata& metadata,
                            GlobalStorageMode storage = GSM_DEFAULT)
{
  bool first = true;
  std::ostringstream body;
  append_symbol_items(body, first, metadata, storage);
  if (!first)
    out << " [" << body.str() << "]";
}

void append_boundary_metadata(std::ostringstream& out,
                              const FunctionBoundaryMetadata& boundary)
{
  bool first = true;
  std::ostringstream body;
  append_boundary_items(body, first, boundary);
  if (!first)
    out << " [" << body.str() << "]";
}

// A function header carries its boundary and symbol metadata in one list.
void append_function_metadata(std::ostringstream& out,
                              const FunctionBoundaryMetadata& boundary,
                              const SymbolMetadata& metadata)
{
  bool first = true;
  std::ostringstream body;
  append_boundary_items(body, first, boundary);
  append_symbol_items(body, first, metadata, GSM_DEFAULT);
  if (!first)
    out << " [" << body.str() << "]";
}

void append_parameter_metadata(std::ostringstream& out,
                               const ParameterMetadata& metadata)
{
  bool first = true;
  std::ostringstream body;
  if (metadata.passing != PPM_DIRECT) {
    const char* value = metadata.passing == PPM_INDIRECT_RESULT ? "indirect_result" :
        metadata.passing == PPM_BY_ADDRESS ? "by_address" :
        metadata.passing == PPM_REFERENCE ? "reference" : "decay";
    body << "pass=" << value; first = false;
  }
  if (metadata.capture != PCM_DEFAULT) { body << (first ? "" : ", ") << "capture=" << (metadata.capture == PCM_NOCAPTURE ? "nocapture" : "maycapture"); first = false; }
  if (metadata.access != PAM_DEFAULT) {
    const char* value = metadata.access == PAM_NONE ? "none" : metadata.access == PAM_READ ? "read" : metadata.access == PAM_WRITE ? "write" : "readwrite";
    body << (first ? "" : ", ") << "access=" << value; first = false;
  }
  if (metadata.alias != PALM_DEFAULT) { body << (first ? "" : ", ") << "alias=noalias"; first = false; }
  if (!first)
    out << " [" << body.str() << "]";
}

void append_call_boundary(std::ostringstream& out,
                          const FunctionBoundaryMetadata& boundary)
{
  // A call signature uses the same boundary grammar as a function header.
  append_boundary_metadata(out, boundary);
}

std::string instruction_text(const Instruction& instruction)
{
  std::ostringstream out;
  const bool assigned = !instruction.dest.empty();
  if (assigned)
    out << instruction.dest << " = ";
  switch (instruction.kind) {
  case Instruction::IK_CONST:
    out << "const " << instruction.type.text << " " << operand(instruction.first);
    break;
  case Instruction::IK_COPY:
    out << "copy " << instruction.type.text << " " << operand(instruction.first);
    break;
  case Instruction::IK_ADDR:
    out << "addr " << operand(instruction.first);
    break;
  case Instruction::IK_LOAD:
    out << "load " << instruction.type.text << " " << operand(instruction.first);
    break;
  case Instruction::IK_STORE:
    out << "store " << instruction.type.text << " " << operand(instruction.first)
        << ", " << operand(instruction.second);
    break;
  case Instruction::IK_COPYOBJ:
    out << "copyobj " << instruction.byte_count << "x"
        << instruction.byte_alignment << " " << operand(instruction.first)
        << ", " << operand(instruction.second);
    break;
  case Instruction::IK_ZEROINIT:
    out << "zeroinit " << instruction.byte_count << "x"
        << instruction.byte_alignment << " " << operand(instruction.first);
    break;
  case Instruction::IK_INDEX:
    out << "index " << instruction.type.text;
    if (instruction.index_projection != IPK_NONE) {
      const char* projection = instruction.index_projection == IPK_ARRAY_ELEMENT ? "array_element" :
          instruction.index_projection == IPK_FIELD ? "field" :
          instruction.index_projection == IPK_BASE_SUBOBJECT ? "base_subobject" : "reference_field";
      out << " [projection=" << projection << "]";
    }
    out << " " << operand(instruction.first) << ", " << operand(instruction.second);
    break;
  case Instruction::IK_UNARY:
    out << "unary " << instruction.op << " " << instruction.type.text << " "
        << operand(instruction.first);
    break;
  case Instruction::IK_BINARY:
    out << "binary " << instruction.op << " " << instruction.type.text << " "
        << operand(instruction.first) << ", " << operand(instruction.second);
    break;
  case Instruction::IK_CMP:
    out << "cmp " << instruction.op << " " << instruction.type.text << " "
        << operand(instruction.first) << ", " << operand(instruction.second);
    break;
  case Instruction::IK_CONVERT:
    out << "convert " << instruction.op << " " << instruction.type.text << " "
        << instruction.source_type.text << " " << operand(instruction.first);
    break;
  case Instruction::IK_CALL:
    out << "call " << instruction.type.text << " " << operand(instruction.first) << "(";
    for (std::size_t i = 0; i < instruction.args.size(); ++i) {
      if (i != 0) out << ", ";
      out << operand(instruction.args[i]);
    }
    out << ")";
    if (instruction.has_call_signature) {
      out << " as (";
      for (std::size_t i = 0; i < instruction.call_params.size(); ++i) {
        if (i != 0) out << ", ";
        out << instruction.call_params[i].name << " : "
            << instruction.call_params[i].type.text;
        std::ostringstream metadata;
        append_parameter_metadata(metadata, instruction.call_params[i].metadata);
        out << metadata.str();
      }
      out << ") -> " << instruction.call_return_type.text;
      std::ostringstream metadata;
      append_call_boundary(metadata, instruction.call_boundary);
      out << metadata.str();
    }
    break;
  case Instruction::IK_JUMP:
    out << "jump " << operand(instruction.first);
    break;
  case Instruction::IK_BRANCH:
    out << "branch " << operand(instruction.first) << ", "
        << operand(instruction.second) << ", " << operand(instruction.third);
    break;
  case Instruction::IK_SWITCH:
    out << "switch " << operand(instruction.first) << ", "
        << operand(instruction.second);
    for (std::size_t i = 0; i + 1 < instruction.args.size(); i += 2)
      out << ", " << operand(instruction.args[i]) << ":"
          << operand(instruction.args[i + 1]);
    break;
  case Instruction::IK_EH_TRY:
    out << "eh_try " << operand(instruction.first);
    break;
  case Instruction::IK_EH_CLEANUP:
    out << "eh_cleanup " << operand(instruction.first);
    break;
  case Instruction::IK_EH_END:
    out << "eh_end";
    break;
  case Instruction::IK_RESUME:
    out << "resume";
    break;
  case Instruction::IK_RETURN:
    out << "return " << instruction.type.text;
    if (!instruction.first.text.empty() || instruction.first.kind != Operand::OP_INTEGER)
      out << " " << operand(instruction.first);
    break;
  default:
    throw std::logic_error("LowIR serializer does not own this instruction yet");
  }
  if (instruction.debug_location.present())
    out << " !dbg (" << instruction.debug_location.file << ", "
        << instruction.debug_location.line << ", "
        << instruction.debug_location.column << ")";
  return out.str();
}

std::string function_text(const Function& function)
{
  std::ostringstream out;
  out << "function " << function.name << "(";
  for (std::size_t i = 0; i < function.params.size(); ++i) {
    if (i != 0) out << ", ";
    out << function.params[i].name << " : " << function.params[i].type.text;
    std::ostringstream metadata;
    append_parameter_metadata(metadata, function.params[i].metadata);
    out << metadata.str();
  }
  out << ") -> " << function.return_type.text;
  append_function_metadata(out, function.boundary, function.metadata);
  if (function.debug_location.present())
    out << " !dbg (" << function.debug_location.file << ", "
        << function.debug_location.line << ", "
        << function.debug_location.column << ")";
  out << " {\n";
  for (std::size_t i = 0; i < function.slots.size(); ++i)
    out << "  slot " << function.slots[i].first << " : "
        << function.slots[i].second.text << "\n";
  if (!function.slots.empty() && !function.blocks.empty())
    out << "\n";
  for (std::size_t b = 0; b < function.blocks.size(); ++b) {
    if (b != 0) out << "\n";
    out << "  block " << function.blocks[b].label << ":\n";
    for (std::size_t i = 0; i < function.blocks[b].instructions.size(); ++i)
      out << "    " << instruction_text(function.blocks[b].instructions[i]) << "\n";
  }
  out << "}\n";
  return out.str();
}

std::string declaration_text(const FunctionDeclaration& declaration)
{
  std::ostringstream out;
  out << "declare function " << declaration.name << "(";
  for (std::size_t i = 0; i < declaration.params.size(); ++i) {
    if (i != 0) out << ", ";
    out << declaration.params[i].name << " : " << declaration.params[i].type.text;
    std::ostringstream metadata;
    append_parameter_metadata(metadata, declaration.params[i].metadata);
    out << metadata.str();
  }
  out << ") -> " << declaration.return_type.text;
  append_function_metadata(out, declaration.boundary, declaration.metadata);
  return out.str();
}

std::string global_declaration_text(const GlobalDeclaration& declaration)
{
  std::ostringstream out;
  out << "declare global " << declaration.name;
  if (declaration.has_type)
    out << " : " << declaration.type.text;
  append_symbol_metadata(out, declaration.metadata, declaration.storage);
  return out.str();
}

std::string global_definition_text(const GlobalDefinition& global)
{
  std::ostringstream out;
  out << "global " << global.name;
  if (global.storage == GSM_READONLY)
    out << " readonly";
  if (!global.structured) {
    out << " : " << global.type.text;
    append_symbol_metadata(out, global.metadata,
                           global.storage == GSM_THREAD_LOCAL ?
                               GSM_THREAD_LOCAL : GSM_DEFAULT);
    out << " = ";
    if (global.init_kind == GlobalDefinition::INIT_ZERO)
      out << "zero";
    else if (global.init_kind == GlobalDefinition::INIT_ADDR) {
      out << "addr " << operand(global.init_operand);
      if (global.addr_addend > 0)
        out << " + " << global.addr_addend;
      else if (global.addr_addend < 0)
        out << " - " << -global.addr_addend;
    }
    else
      out << operand(global.init_operand);
    return out.str();
  }
  append_symbol_metadata(out, global.metadata,
                         global.storage == GSM_THREAD_LOCAL ?
                             GSM_THREAD_LOCAL : GSM_DEFAULT);
  out << " = {\n";
  for (std::size_t i = 0; i < global.data_items.size(); ++i) {
    const GlobalDefinition::DataItem& item = global.data_items[i];
    out << "  ";
    if (item.kind == GlobalDefinition::DataItem::ITEM_ZERO) {
      out << "zero " << item.zero_bytes;
    } else if (item.kind == GlobalDefinition::DataItem::ITEM_ADDR) {
      out << item.type.text << " addr " << item.symbol;
      if (item.addr_addend > 0)
        out << "+" << item.addr_addend;
      else if (item.addr_addend < 0)
        out << item.addr_addend;
    } else {
      out << item.type.text << " " << operand(item.literal_operand);
    }
    out << "\n";
  }
  out << "}";
  return out.str();
}

}  // namespace

// Canonical top-level layout (lowir.md, Program Structure): declarations,
// then global definitions, then functions, with one blank line between the
// groups that are present and none inside a group.
std::string serialize_lowir_program(const Program& program)
{
  std::string declarations;
  for (std::size_t i = 0; i < program.global_declarations.size(); ++i)
    declarations += global_declaration_text(program.global_declarations[i]) + "\n";
  for (std::size_t i = 0; i < program.function_declarations.size(); ++i)
    declarations += declaration_text(program.function_declarations[i]) + "\n";
  std::string globals;
  for (std::size_t i = 0; i < program.globals.size(); ++i)
    globals += global_definition_text(program.globals[i]) + "\n";
  std::string functions;
  for (std::size_t i = 0; i < program.functions.size(); ++i)
    functions += function_text(program.functions[i]);
  for (std::size_t i = 0; i < program.object_aliases.size(); ++i)
    functions += "alias object " + program.object_aliases[i].object_symbol +
        " = " + program.object_aliases[i].target + "\n";
  const std::string* groups[] = { &declarations, &globals, &functions };
  std::string out;
  for (std::size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
    if (groups[i]->empty())
      continue;
    if (!out.empty())
      out += "\n";
    out += *groups[i];
  }
  return out;
}

}  // namespace lowir_model
