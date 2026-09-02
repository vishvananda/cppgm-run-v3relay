#include "lowir_cy86_codegen.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using lowir_model::Function;
using lowir_model::Instruction;
using lowir_model::LowType;
using lowir_model::LowTypeInfo;
using lowir_model::Operand;
using lowir_model::Parameter;
using lowir_model::Program;

// Width facts.  Every size, alignment and operand-width decision derives from
// `describe_low_type`; the helpers below only phrase that table for CY86.

bool is_f80(const LowTypeInfo & info)
{
  return info.floating() && info.bits == 80;
}

// Instructions without a type (jumps, fences, bulk copies) carry an empty
// spelling; they are never f80 or wide.
bool is_f80(const LowType & type)
{
  return !type.text.empty() && is_f80(lowir_model::describe_low_type(type));
}

// Wide values (`f80` and direct objects) live in memory and cross call
// boundaries by address; everything else is one 64-bit register.
bool is_wide(const LowTypeInfo & info)
{
  return info.object() || is_f80(info);
}

bool is_wide(const LowType & type)
{
  return !type.text.empty() && is_wide(lowir_model::describe_low_type(type));
}

std::size_t value_size(const LowTypeInfo & info)
{
  return is_wide(info) ? (info.bytes + 7) & ~static_cast<std::size_t>(7) : 8;
}

std::string width_suffix(const LowTypeInfo & info)
{
  if(info.integer() || info.floating()) {
    if(info.bits <= 8) return "8";
    if(info.bits <= 16) return "16";
    if(info.bits <= 32) return "32";
  }
  return "64";
}

std::string width_suffix(const LowType & type)
{
  return width_suffix(lowir_model::describe_low_type(type));
}

std::string number(std::size_t value)
{
  return std::to_string(value);
}

std::string signed_number(long long value)
{
  return std::to_string(value);
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

// A CY86 memory operand: a base register plus a signed displacement.
struct Address
{
  const char * base;
  long long offset;

  Address(const char * base_register, long long displacement = 0)
    : base(base_register), offset(displacement)
  {}

  Address plus(long long delta) const
  {
    return Address(base, offset + delta);
  }
};

std::string memory(const Address & address)
{
  std::string text = "[";
  text += address.base;
  if(address.offset > 0) text += "+";
  if(address.offset != 0) text += signed_number(address.offset);
  text += "]";
  return text;
}

std::string literal_text(const Operand & operand)
{
  return operand.text == "nullptr" ? "0" : operand.text;
}

class Cy86Writer
{
public:
  void instruction(const std::string & op, const std::string & operands)
  {
    out_ += '\t';
    out_ += op;
    if(!operands.empty()) {
      out_ += ' ';
      out_ += operands;
    }
    out_ += ";\n";
  }

  void label(const std::string & name)
  {
    out_ += name;
    out_ += ":\n";
  }

  void blank()
  {
    out_ += '\n';
  }

  std::string take()
  {
    return std::move(out_);
  }

private:
  std::string out_;
};

struct ValueInfo
{
  long long offset = 0;
  LowType type;
  LowTypeInfo info;
};

// Frame layout of one function: parameters (preceded by the hidden result
// pointer of a wide return), slots, then temporaries in definition order,
// each at a negative offset from `bp`; wide instructions add scratch areas
// below the locals.
struct FrameLayout
{
  std::unordered_map<std::string, ValueInfo> values;
  std::vector<std::string> abi_params;
  std::size_t local_size = 0;
  std::size_t frame_size = 0;
  bool has_scratch = false;
  std::string hidden_result;
};

bool instruction_needs_scratch(const Instruction & instruction)
{
  if(instruction.kind == Instruction::IK_CONVERT) return true;
  if(is_f80(instruction.type) || is_f80(instruction.source_type) ||
     is_f80(instruction.call_return_type)) {
    return true;
  }
  for(std::size_t i = 0; i < instruction.call_params.size(); ++i) {
    if(is_f80(instruction.call_params[i].type)) return true;
  }
  return false;
}

void add_value(FrameLayout & layout, const std::string & name,
               const LowType & type, std::size_t & used)
{
  ValueInfo value;
  value.type = type;
  value.info = lowir_model::describe_low_type(type);
  used += value_size(value.info);
  value.offset = -static_cast<long long>(used);
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
    layout.abi_params.push_back(layout.hidden_result);
    add_value(layout, layout.hidden_result, pointer, used);
  }
  for(std::size_t i = 0; i < function.params.size(); ++i) {
    layout.abi_params.push_back(function.params[i].name);
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
        add_value(layout, instruction.dest,
                  lowir_model::instruction_result_type(instruction), used);
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

  const Program & program() const
  {
    return program_;
  }

  const LowirProgramFacts & facts() const
  {
    return facts_;
  }

  std::string symbol_label(const std::string & name) const
  {
    return facts_.is_function(name) ? function_label(name) : global_label(name);
  }

  std::string next_label(const std::string & prefix)
  {
    return prefix + number(label_counter_++);
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
      layout_(build_frame(function)), label_prefix_(function_label(function.name))
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
  std::string label_prefix_;

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
    return label_prefix_ + "__" + strip_sigil(name);
  }

  std::string epilogue_label() const
  {
    return label_prefix_ + "__epilogue";
  }

  static std::string parameter_register(std::size_t index)
  {
    static const char * const names[] = { "x", "y", "z", "t" };
    return index < 4 ? names[index] : "x";
  }

  // Scratch areas SA, SB, SC below the locals; each holds one 16-byte value.
  Address scratch(std::size_t area) const
  {
    return Address("bp", -static_cast<long long>(layout_.local_size + 16 * (area + 1)));
  }

  static Address local(const ValueInfo & value)
  {
    return Address("bp", value.offset);
  }

  // Register `r` <- address of a local value (`isub64 r64 bp N`).
  void load_local_address(const ValueInfo & value, const std::string & base)
  {
    writer_.instruction("isub64", base + "64 bp " +
                        number(static_cast<std::size_t>(-value.offset)));
  }

  void pad_f80(const Address & target)
  {
    writer_.instruction("move64", "z64 0");
    writer_.instruction("move32", memory(target.plus(10)) + " z32");
    writer_.instruction("move16", memory(target.plus(14)) + " z16");
  }

  void copy_memory(const Address & source, const Address & target, std::size_t bytes)
  {
    for(std::size_t offset = 0; offset < bytes; offset += 8) {
      const long long delta = static_cast<long long>(offset);
      writer_.instruction("move64", "z64 " + memory(source.plus(delta)));
      writer_.instruction("move64", memory(target.plus(delta)) + " z64");
    }
  }

  void emit_prologue()
  {
    writer_.label(label_prefix_);
    writer_.instruction("isub64", "sp sp 8");
    writer_.instruction("move64", "[sp] bp");
    writer_.instruction("move64", "bp sp");
    if(layout_.frame_size != 0) {
      writer_.instruction("isub64", "sp sp " + number(layout_.frame_size));
    }
    for(std::size_t i = 0; i < layout_.abi_params.size(); ++i) {
      emit_parameter_spill(i, value(layout_.abi_params[i]));
    }
  }

  // Register parameters arrive in x, y, z, t; later ones on the caller's
  // stack above the saved bp and return address.  Wide parameters arrive as
  // pointers and are copied into their local storage.
  void emit_parameter_spill(std::size_t index, const ValueInfo & target)
  {
    if(is_wide(target.info)) {
      if(index < 4) {
        writer_.instruction("move64", "x64 " + parameter_register(index) + "64");
      } else {
        writer_.instruction("move64", "x64 " + memory(incoming_argument(index)));
      }
      copy_memory(Address("x64"), local(target), value_size(target.info));
      return;
    }
    if(index < 4) {
      writer_.instruction("move64", memory(local(target)) + " " +
                          parameter_register(index) + "64");
    } else {
      writer_.instruction("move64", "x64 " + memory(incoming_argument(index)));
      writer_.instruction("move64", memory(local(target)) + " x64");
    }
  }

  // Stack argument k (the fifth and later) sits above the saved bp and the
  // return address.
  static Address incoming_argument(std::size_t index)
  {
    return Address("bp", 16 + static_cast<long long>((index - 4) * 8));
  }

  void emit_epilogue()
  {
    writer_.label(epilogue_label());
    writer_.instruction("move64", "sp bp");
    writer_.instruction("move64", "bp [sp]");
    writer_.instruction("iadd64", "sp sp 8");
    writer_.instruction("ret", "");
  }

  // The type an argument operand carries when no parameter type is known.
  std::string operand_type_name(const Operand & operand,
                                const std::string & fallback) const
  {
    if(operand.kind == Operand::OP_TEMP || operand.kind == Operand::OP_SLOT) {
      return value(operand.text).type.text;
    }
    if(operand.kind == Operand::OP_GLOBAL && context_.facts().is_function(operand.text)) {
      return "ptr";
    }
    if(!operand.literal_type.text.empty()) return operand.literal_type.text;
    return fallback;
  }

  // Register `base` <- the operand as a value: a scalar temporary is read at
  // `type`'s width (its own width when `type` is empty), a slot, wide
  // temporary or symbol yields its address, a literal is moved verbatim.
  void load_value(const Operand & operand, const LowType & type,
                  const std::string & base)
  {
    if(operand.kind == Operand::OP_TEMP) {
      const ValueInfo & source = value(operand.text);
      if(is_f80(source.info)) {
        load_local_address(source, base);
        return;
      }
      const std::string suffix = type.text.empty() ? width_suffix(source.info) :
                                                     width_suffix(type);
      if(suffix == "8" || suffix == "16") {
        writer_.instruction("move64", base + "64 0");
      }
      writer_.instruction("move" + suffix,
                          base + suffix + " " + memory(local(source)));
      return;
    }
    if(operand.kind == Operand::OP_SLOT) {
      load_local_address(value(operand.text), base);
      return;
    }
    if(operand.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move64", base + "64 " + context_.symbol_label(operand.text));
      return;
    }
    const std::string suffix = type.text == "f32" ? "32" : "64";
    writer_.instruction("move" + suffix, base + suffix + " " + literal_text(operand));
  }

  // Register `base` <- the address of the operand's storage.
  void load_address(const Operand & operand, const std::string & base)
  {
    if(operand.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move64", base + "64 " + context_.symbol_label(operand.text));
    } else if(operand.kind == Operand::OP_TEMP || operand.kind == Operand::OP_SLOT) {
      load_local_address(value(operand.text), base);
    } else {
      load_value(operand, LowType(), base);
    }
  }

  // Register `base` <- the address an operand designates as storage: a
  // scalar temporary holds a pointer value, anything else is addressed.
  void load_storage_address(const Operand & operand, const std::string & base)
  {
    if(operand.kind == Operand::OP_TEMP && !is_wide(value(operand.text).info)) {
      LowType pointer;
      pointer.text = "ptr";
      load_value(operand, pointer, base);
    } else {
      load_address(operand, base);
    }
  }

  void stage_f80_operand(const Operand & operand, std::size_t area)
  {
    if(operand.kind == Operand::OP_FLOAT) {
      writer_.instruction("move80", memory(scratch(area)) + " " + literal_text(operand));
      pad_f80(scratch(area));
      return;
    }
    load_address(operand, "x");
    copy_memory(Address("x64"), scratch(area), 16);
  }

  void store_wide_from_scratch(const std::string & destination, std::size_t area)
  {
    copy_memory(scratch(area), local(value(destination)), 16);
  }

  void copy_wide_operand_to_value(const Operand & source,
                                  const std::string & destination,
                                  const LowType & type)
  {
    const ValueInfo & target = value(destination);
    if(source.kind == Operand::OP_FLOAT && is_f80(type)) {
      stage_f80_operand(source, 0);
      copy_memory(scratch(0), local(target), 16);
      return;
    }
    load_address(source, "x");
    copy_memory(Address("x64"), local(target), value_size(target.info));
  }

  void store_result(const std::string & destination, const LowType & type,
                    const std::string & base)
  {
    const ValueInfo & target = value(destination);
    if(is_wide(target.info)) {
      throw std::runtime_error("scalar store into wide LowIR value " + destination);
    }
    const std::string suffix = width_suffix(type);
    writer_.instruction("move" + suffix, memory(local(target)) + " " + base + suffix);
  }

  void emit_const(const Instruction & instruction)
  {
    if(is_f80(instruction.type)) {
      stage_f80_operand(instruction.first, 0);
      store_wide_from_scratch(instruction.dest, 0);
      return;
    }
    load_value(instruction.first, instruction.type, "x");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_copy(const Instruction & instruction)
  {
    if(is_wide(instruction.type)) {
      copy_wide_operand_to_value(instruction.first, instruction.dest,
                                 instruction.type);
      return;
    }
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
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    if(is_wide(info)) {
      if(is_f80(info)) {
        load_storage_address(instruction.first, "x");
        copy_memory(Address("x64"), scratch(0), 16);
        store_wide_from_scratch(instruction.dest, 0);
      } else {
        load_storage_address(instruction.first, "x");
        copy_memory(Address("x64"), local(value(instruction.dest)), value_size(info));
      }
      return;
    }
    const std::string suffix = width_suffix(info);
    if(instruction.first.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move" + suffix,
                          "x" + suffix + " [" + global_label(instruction.first.text) + "]");
    } else if(instruction.first.kind == Operand::OP_SLOT) {
      writer_.instruction("move" + suffix,
                          "x" + suffix + " " + memory(local(value(instruction.first.text))));
    } else {
      load_value(instruction.first, LowType(), "x");
      writer_.instruction("move" + suffix, "x" + suffix + " [x64]");
      // A 32-bit signed load through a pointer temporary is sign-extended
      // in the register before it is stored (pinned CY86 shape).
      if(instruction.type.text == "i32" && instruction.first.kind == Operand::OP_TEMP) {
        writer_.instruction("move8", "t8 32");
        writer_.instruction("lshift64", "x64 x64 t8");
        writer_.instruction("srshift64", "x64 x64 t8");
      }
    }
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_store(const Instruction & instruction)
  {
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    if(is_wide(info)) {
      if(is_f80(info)) {
        stage_f80_operand(instruction.first, 0);
        load_storage_address(instruction.second, "y");
        copy_memory(scratch(0), Address("y64"), 16);
      } else {
        load_address(instruction.first, "x");
        load_storage_address(instruction.second, "y");
        copy_memory(Address("x64"), Address("y64"), value_size(info));
      }
      return;
    }
    const std::string suffix = width_suffix(info);
    load_value(instruction.first, instruction.type, "x");
    if(instruction.second.kind == Operand::OP_GLOBAL) {
      writer_.instruction("move" + suffix,
                          "[" + global_label(instruction.second.text) + "] x" + suffix);
    } else if(instruction.second.kind == Operand::OP_SLOT) {
      writer_.instruction("move" + suffix,
                          memory(local(value(instruction.second.text))) + " x" + suffix);
    } else {
      load_value(instruction.second, LowType(), "y");
      writer_.instruction("move" + suffix, "[y64] x" + suffix);
    }
  }

  void emit_binary(const Instruction & instruction)
  {
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    if(is_f80(info)) {
      stage_f80_operand(instruction.first, 0);
      stage_f80_operand(instruction.second, 1);
      writer_.instruction("f" + instruction.op + "80",
                          memory(scratch(2)) + " " + memory(scratch(0)) +
                          " " + memory(scratch(1)));
      pad_f80(scratch(2));
      store_wide_from_scratch(instruction.dest, 2);
      return;
    }
    const std::string suffix = width_suffix(info);
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

  static std::string binary_opcode(const std::string & op)
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

  // Every comparison materializes a canonical i64 truth value.
  void emit_compare(const Instruction & instruction)
  {
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    if(is_f80(info)) {
      stage_f80_operand(instruction.first, 0);
      stage_f80_operand(instruction.second, 1);
      writer_.instruction("f" + instruction.op + "80", "z8 " +
                          memory(scratch(0)) + " " + memory(scratch(1)));
    } else {
      const std::string suffix = width_suffix(info);
      load_value(instruction.first, instruction.type, "y");
      load_value(instruction.second, instruction.type, "x");
      writer_.instruction((info.floating() ? "f" + instruction.op :
                           compare_opcode(instruction.op)) + suffix,
                          "z8 y" + suffix + " x" + suffix);
    }
    writer_.instruction("move64", "x64 0");
    writer_.instruction("move8", "x8 z8");
    store_result(instruction.dest, lowir_model::instruction_result_type(instruction), "x");
  }

  static std::string compare_opcode(const std::string & op)
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
    if(is_f80(instruction.type)) {
      if(instruction.op != "neg") {
        throw std::runtime_error("unsupported f80 unary operation " + instruction.op);
      }
      stage_f80_operand(instruction.first, 0);
      writer_.instruction("move80", memory(scratch(1)) + " 0.0L");
      pad_f80(scratch(1));
      writer_.instruction("fsub80", memory(scratch(2)) + " " +
                          memory(scratch(1)) + " " + memory(scratch(0)));
      pad_f80(scratch(2));
      store_wide_from_scratch(instruction.dest, 2);
      return;
    }
    const std::string suffix = width_suffix(instruction.type);
    load_value(instruction.first, instruction.type, "x");
    if(instruction.op == "neg") {
      writer_.instruction("move" + suffix, "y" + suffix + " 0");
      writer_.instruction("isub" + suffix,
                          "x" + suffix + " y" + suffix + " x" + suffix);
    } else if(instruction.op == "not") {
      writer_.instruction("ieq64", "z8 x64 0");
      writer_.instruction("move64", "x64 0");
      writer_.instruction("move8", "x8 z8");
    } else if(instruction.op == "bitnot") {
      writer_.instruction("not" + suffix, "x" + suffix + " x" + suffix);
    } else if(instruction.op == "bswap") {
      writer_.instruction("bswap" + suffix, "x" + suffix + " x" + suffix);
    } else if(instruction.op != "decay") {
      throw std::runtime_error("unsupported unary operation " + instruction.op);
    }
    store_result(instruction.dest, instruction.type, "x");
  }

  // Integer/float conversions go through an f80 staging area: the source is
  // widened into SA, then narrowed from SA into the destination.
  void emit_convert(const Instruction & instruction)
  {
    if(instruction.op == "zext" || instruction.op == "sext" ||
       instruction.op == "trunc") {
      load_value(instruction.first, instruction.source_type, "x");
      if(instruction.op == "sext") {
        const LowTypeInfo source = lowir_model::describe_low_type(instruction.source_type);
        writer_.instruction("move8", "t8 " + number(64 - source.bits));
        writer_.instruction("lshift64", "x64 x64 t8");
        writer_.instruction("srshift64", "x64 x64 t8");
      }
      store_result(instruction.dest, instruction.type, "x");
      return;
    }

    const LowTypeInfo source = lowir_model::describe_low_type(instruction.source_type);
    const LowTypeInfo destination = lowir_model::describe_low_type(instruction.type);
    if(is_f80(source)) {
      stage_f80_operand(instruction.first, 0);
    } else {
      load_value(instruction.first, instruction.source_type, "x");
      const std::string widen = source.floating() ? instruction.source_type.text :
        std::string(instruction.op == "uitofp" ? "u" : "s") + number(source.bits);
      writer_.instruction(widen + "convf80",
                          memory(scratch(0)) + " x" + width_suffix(source));
      pad_f80(scratch(0));
    }

    if(is_f80(destination)) {
      store_wide_from_scratch(instruction.dest, 0);
      return;
    }
    const std::string narrow = destination.floating() ? instruction.type.text :
      std::string(instruction.op == "fptoui" ? "u" : "s") + number(destination.bits);
    writer_.instruction("f80conv" + narrow,
                        memory(local(value(instruction.dest))) + " " + memory(scratch(0)));
  }

  void emit_index(const Instruction & instruction)
  {
    load_value(instruction.first, LowType(), "y");
    LowType index_type;
    index_type.text = "i64";
    load_value(instruction.second, index_type, "x");
    const std::size_t element_size = lowir_model::describe_low_type(instruction.type).bytes;
    if(element_size != 1) {
      writer_.instruction("move64", "z64 " + number(element_size));
      writer_.instruction("smul64", "x64 x64 z64");
    }
    writer_.instruction("iadd64", "x64 y64 x64");
    store_result(instruction.dest, lowir_model::instruction_result_type(instruction), "x");
  }

  void emit_copyobj(const Instruction & instruction)
  {
    load_storage_address(instruction.second, "x");
    load_storage_address(instruction.first, "y");
    for(std::size_t offset = 0; offset < instruction.byte_count; offset += 8) {
      writer_.instruction("move64", "z64 [y64]");
      writer_.instruction("move64", "[x64] z64");
      if(offset + 8 < instruction.byte_count) {
        writer_.instruction("iadd64", "x64 x64 8");
        writer_.instruction("iadd64", "y64 y64 8");
      }
    }
  }

  void emit_zeroinit(const Instruction & instruction)
  {
    load_storage_address(instruction.first, "x");
    writer_.instruction("move64", "z64 0");
    for(std::size_t offset = 0; offset < instruction.byte_count; offset += 8) {
      writer_.instruction("move64", "[x64] z64");
      if(offset + 8 < instruction.byte_count) {
        writer_.instruction("iadd64", "x64 x64 8");
      }
    }
  }

  // The parameter list a call binds against: its explicit signature, else the
  // callee's definition or declaration.
  const std::vector<Parameter> * call_parameters(const Instruction & instruction) const
  {
    if(instruction.has_call_signature) return &instruction.call_params;
    return context_.facts().callee_parameters(context_.program(), instruction.first.text);
  }

  LowType call_parameter_type(const std::vector<Parameter> * parameters,
                              const Operand & argument, std::size_t index) const
  {
    LowType type;
    if(parameters != 0 && index < parameters->size()) return (*parameters)[index].type;
    type.text = operand_type_name(argument, "i64");
    return type;
  }

  bool argument_is_address(const Operand & operand, const LowType & expected) const
  {
    if(is_wide(expected)) return true;
    return operand.kind == Operand::OP_SLOT ||
           (operand.kind == Operand::OP_TEMP && is_wide(value(operand.text).info));
  }

  // Register `target` <- one call argument: wide values, slots and wide
  // temporaries pass their address; scalars pass the value.
  void emit_call_argument(const Operand & operand, const LowType & expected,
                          const std::string & target)
  {
    if(!argument_is_address(operand, expected)) {
      load_value(operand, expected, target);
      return;
    }
    if(operand.kind == Operand::OP_FLOAT && is_f80(expected)) {
      stage_f80_operand(operand, 0);
      writer_.instruction("isub64", "x64 bp " + number(layout_.local_size + 16));
    } else {
      load_address(operand, "x");
    }
    writer_.instruction("move64", target + "64 x64");
  }

  // Register `target` <- where the callee writes a wide result: the
  // destination temporary, or scratch SA when the result is discarded.
  void emit_hidden_result_argument(const Instruction & instruction,
                                   const std::string & target)
  {
    if(!instruction.dest.empty()) {
      load_local_address(value(instruction.dest), "x");
    } else {
      writer_.instruction("isub64", "x64 bp " + number(layout_.local_size + 16));
    }
    writer_.instruction("move64", target + "64 x64");
  }

  void emit_call(const Instruction & instruction)
  {
    const std::vector<Parameter> * parameters = call_parameters(instruction);
    const bool indirect = instruction.first.kind != Operand::OP_GLOBAL;
    const bool hidden_result = is_wide(instruction.type);
    const std::size_t abi_argument_count = instruction.args.size() +
                                            (hidden_result ? 1 : 0);
    const std::size_t stack_bytes = abi_argument_count > 4 ?
      (abi_argument_count - 4) * 8 : 0;
    if(indirect) {
      load_value(instruction.first, LowType(), "x");
      writer_.instruction("isub64", "sp sp " + number(stack_bytes + 8));
      writer_.instruction("move64", memory(Address("sp", static_cast<long long>(stack_bytes))) + " x64");
    } else if(stack_bytes != 0) {
      writer_.instruction("isub64", "sp sp " + number(stack_bytes));
    }
    for(std::size_t abi_index = 0; abi_index < abi_argument_count; ++abi_index) {
      const bool in_registers = abi_index < 4;
      const std::string target = in_registers ? parameter_register(abi_index) : "x";
      if(hidden_result && abi_index == 0) {
        emit_hidden_result_argument(instruction, target);
        continue;
      }
      const std::size_t argument_index = abi_index - (hidden_result ? 1 : 0);
      const Operand & argument = instruction.args[argument_index];
      emit_call_argument(argument, call_parameter_type(parameters, argument, argument_index),
                         target);
      if(!in_registers) {
        const Address outgoing("sp", static_cast<long long>((abi_index - 4) * 8));
        writer_.instruction("move64", memory(outgoing) + " 0");
        writer_.instruction("move64", memory(outgoing) + " x64");
      }
    }
    if(indirect) {
      writer_.instruction("call", memory(Address("sp", static_cast<long long>(stack_bytes))));
      writer_.instruction("iadd64", "sp sp " + number(stack_bytes + 8));
    } else {
      writer_.instruction("call", function_label(instruction.first.text));
      if(stack_bytes != 0) {
        writer_.instruction("iadd64", "sp sp " + number(stack_bytes));
      }
    }
    if(!instruction.dest.empty() && !hidden_result) {
      store_result(instruction.dest, instruction.type, "x");
    }
  }

  void emit_atomic_load(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    load_value(instruction.first, LowType(), "y");
    writer_.instruction("move" + suffix, "x" + suffix + " [y64]");
    store_result(instruction.dest, instruction.type, "x");
  }

  void emit_atomic_store(const Instruction & instruction)
  {
    const std::string suffix = width_suffix(instruction.type);
    load_value(instruction.second, LowType(), "y");
    load_value(instruction.first, instruction.type, "x");
    writer_.instruction("move" + suffix, "[y64] x" + suffix);
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
      writer_.instruction("jumpif", "z8 " + block_label(instruction.args[i + 1].text));
    }
    writer_.instruction("jump", block_label(instruction.second.text));
  }

  // Push a handler record {previous top, handler, bp, sp after pop} onto the
  // stack and make it the exception top.
  void emit_eh_push(const Instruction & instruction)
  {
    writer_.instruction("isub64", "sp sp 32");
    writer_.instruction("move64", "z64 [g____cppgm_eh_top]");
    writer_.instruction("move64", "[sp] z64");
    writer_.instruction("move64", "z64 " + block_label(instruction.first.text));
    writer_.instruction("move64", "[sp+8] z64");
    writer_.instruction("move64", "[sp+16] bp");
    writer_.instruction("move64", "z64 sp");
    writer_.instruction("iadd64", "z64 z64 32");
    writer_.instruction("move64", "[sp+24] z64");
    writer_.instruction("move64", "z64 sp");
    writer_.instruction("move64", "[g____cppgm_eh_top] z64");
  }

  void emit_eh_end()
  {
    writer_.instruction("move64", "x64 [g____cppgm_eh_top]");
    writer_.instruction("move64", "y64 [x64]");
    writer_.instruction("move64", "[g____cppgm_eh_top] y64");
    writer_.instruction("move64", "sp x64");
    writer_.instruction("iadd64", "sp sp 32");
  }

  // Pop the top handler record and transfer to it, or terminate when none.
  void emit_eh_dispatch()
  {
    const std::string handler = context_.next_label("__eh_handler__");
    const std::string unhandled = context_.next_label("__eh_unhandled__");
    writer_.instruction("move64", "x64 [g____cppgm_eh_top]");
    writer_.instruction("ieq64", "z8 x64 0");
    writer_.instruction("jumpif", "z8 " + unhandled);
    writer_.label(handler);
    writer_.instruction("move64", "y64 [x64]");
    writer_.instruction("move64", "[g____cppgm_eh_top] y64");
    writer_.instruction("move64", "z64 [x64+8]");
    writer_.instruction("move64", "bp [x64+16]");
    writer_.instruction("move64", "sp [x64+24]");
    writer_.instruction("jump", "z64");
    writer_.label(unhandled);
    writer_.instruction("move64", "x64 [g____cppgm_eh_value]");
    writer_.instruction("call", "fn____cppgm_eh_unhandled");
    writer_.instruction("syscall1", "t64 60 x64");
    writer_.blank();
  }

  void emit_return(const Instruction & instruction)
  {
    const LowTypeInfo info = lowir_model::describe_low_type(instruction.type);
    if(is_wide(info)) {
      if(layout_.hidden_result.empty()) {
        throw std::runtime_error("wide return is missing its hidden result pointer");
      }
      const ValueInfo & hidden = value(layout_.hidden_result);
      if(is_f80(info)) {
        stage_f80_operand(instruction.first, 0);
        writer_.instruction("move64", "x64 " + memory(local(hidden)));
        copy_memory(scratch(0), Address("x64"), 16);
      } else {
        load_address(instruction.first, "x");
        writer_.instruction("move64", "y64 " + memory(local(hidden)));
        copy_memory(Address("x64"), Address("y64"), value_size(info));
      }
    } else if(info.kind != LowTypeInfo::LTI_VOID) {
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
      case Instruction::IK_EH_TRY:
      case Instruction::IK_EH_CLEANUP:
        emit_eh_push(instruction);
        break;
      case Instruction::IK_EH_END: emit_eh_end(); break;
      case Instruction::IK_THROW:
        load_value(instruction.first, instruction.type, "x");
        writer_.instruction("move64", "[g____cppgm_eh_value] x64");
        emit_eh_dispatch();
        break;
      case Instruction::IK_EXCEPTION:
      case Instruction::IK_EXCEPTION_SELECTOR:
        writer_.instruction("move64", "x64 [g____cppgm_eh_value]");
        store_result(instruction.dest, instruction.type, "x");
        break;
      case Instruction::IK_RESUME: emit_eh_dispatch(); break;
      case Instruction::IK_EH_CLEANUP_CLAUSE:
      case Instruction::IK_EH_CATCH:
      case Instruction::IK_EH_FILTER:
      case Instruction::IK_EH_CATCH_ALL:
        break;
      case Instruction::IK_RETURN: emit_return(instruction); break;
      default:
        throw std::runtime_error("LowIR instruction outside the PA13 CY86 adapter contract");
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

// An f80 datum is its ten extended-precision bytes padded to sixteen: the
// low eight as one data64, the next two as one data16, then six zero bytes.
void emit_f80_data(long double value, Cy86Writer & writer)
{
  unsigned char bytes[sizeof(long double)] = {};
  std::memcpy(bytes, &value, sizeof(value));
  std::int64_t low = 0;
  std::uint16_t high = 0;
  std::memcpy(&low, bytes, sizeof(low));
  std::memcpy(&high, bytes + sizeof(low), sizeof(high));
  writer.instruction("data64", signed_number(static_cast<long long>(low)));
  writer.instruction("data16", number(static_cast<std::size_t>(high)));
  for(std::size_t i = 0; i < 6; ++i) {
    writer.instruction("data8", "0");
  }
}

std::string address_initializer(const CodegenContext & context,
                                const std::string & symbol, long long addend)
{
  std::string text = context.symbol_label(symbol);
  if(addend > 0) text += "+";
  if(addend != 0) text += signed_number(addend);
  return text;
}

void emit_global_scalar(const lowir_model::GlobalDefinition & global,
                        const CodegenContext & context, Cy86Writer & writer)
{
  writer.label(global_label(global.name));
  if(is_f80(global.type)) {
    emit_f80_data(global.init_kind == lowir_model::GlobalDefinition::INIT_ZERO ?
                  0.0L : global.init_operand.float_value, writer);
    return;
  }
  if(global.init_kind == lowir_model::GlobalDefinition::INIT_ZERO) {
    writer.instruction("data" + width_suffix(global.type), "0");
  } else if(global.init_kind == lowir_model::GlobalDefinition::INIT_ADDR) {
    writer.instruction("data64", address_initializer(context, global.init_operand.text,
                                                     global.addr_addend));
  } else {
    writer.instruction("data" + width_suffix(global.type),
                       literal_text(global.init_operand));
  }
}

// Structured data: each item is padded up to its natural alignment.
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
    const std::size_t padding = info.alignment == 0 ? 0 :
      (info.alignment - (offset % info.alignment)) % info.alignment;
    for(std::size_t j = 0; j < padding; ++j) {
      writer.instruction("data8", "0");
    }
    offset += padding;
    if(item.kind == lowir_model::GlobalDefinition::DataItem::ITEM_ADDR) {
      writer.instruction("data64", address_initializer(context, item.symbol,
                                                       item.addr_addend));
    } else if(is_f80(info)) {
      emit_f80_data(item.literal_operand.float_value, writer);
    } else {
      writer.instruction("data" + width_suffix(info), literal_text(item.literal_operand));
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

void emit_eh_runtime(Cy86Writer & writer)
{
  writer.label("fn____cppgm_eh_unhandled");
  writer.instruction("syscall1", "t64 60 x64");
}

void emit_eh_globals(Cy86Writer & writer)
{
  writer.label("g____cppgm_eh_top");
  writer.instruction("data64", "0");
  writer.blank();
  writer.label("g____cppgm_eh_value");
  writer.instruction("data64", "0");
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
  if(facts.uses_eh) {
    writer.blank();
    emit_eh_runtime(writer);
  }
  emit_globals(program, context, writer);
  if(facts.uses_eh) {
    writer.blank();
    emit_eh_globals(writer);
  }
  return writer.take();
}
