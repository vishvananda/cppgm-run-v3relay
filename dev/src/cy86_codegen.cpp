#include "cy86_codegen.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace
{

const uint64_t kImageBase = 0x400000;
const size_t kElfHeaderSize = 64;
const size_t kProgramHeaderSize = 56;
const size_t kImageHeaderSize = kElfHeaderSize + kProgramHeaderSize;

typedef vector<unsigned char> ByteVector;

// Each translated instruction may reuse these 16-byte slots.  They live in
// the System V red zone, so no stack-pointer adjustment is needed and the
// fixed CY86 register convention remains intact.
const int64_t kBouncePrimary = -16;
const int64_t kBounceSecondary = -32;
const int64_t kBounceTertiary = -48;

void Append(ByteVector& output, const ByteVector& bytes)
{
	output.insert(output.end(), bytes.begin(), bytes.end());
}

vector<X86Operand> One(const X86Operand& first)
{
	vector<X86Operand> result;
	result.push_back(first);
	return result;
}

vector<X86Operand> Two(const X86Operand& first, const X86Operand& second)
{
	vector<X86Operand> result;
	result.push_back(first);
	result.push_back(second);
	return result;
}

void Emit(ByteVector& output, X86Mnemonic mnemonic, unsigned width,
	const vector<X86Operand>& operands)
{
	const X86Instruction instruction(mnemonic, width, operands);
	Append(output, instruction.Encode());
}

void Emit(ByteVector& output, X86Mnemonic mnemonic, unsigned width,
	X86Condition condition, const vector<X86Operand>& operands)
{
	const X86Instruction instruction(mnemonic, width, condition, operands);
	Append(output, instruction.Encode());
}

uint64_t WidthMask(unsigned width)
{
	if (width >= 64)
		return numeric_limits<uint64_t>::max();
	return (uint64_t(1) << width) - 1;
}

uint64_t ReadLiteral(const Cy86Literal& literal)
{
	uint64_t value = 0;
	const size_t count = min<size_t>(literal.bytes.size(), sizeof(value));
	for (size_t i = 0; i < count; ++i)
		value |= uint64_t(literal.bytes[i]) << (i * 8);
	return value;
}

uint64_t ConvertLiteral(const Cy86Literal& literal, unsigned width,
	bool negated)
{
	const bool packed_character_array = literal.is_array &&
		literal.type == FT_CHAR && literal.num_elements == literal.bytes.size();
	if (!Cy86IsIntegral(literal.type) ||
		(!packed_character_array &&
			(literal.is_array || literal.num_elements > 1)) ||
		(packed_character_array && literal.bytes.size() > sizeof(uint64_t)) ||
		width == 0 || width > 64)
		throw Cy86Error("literal cannot be used as an integer immediate");
	const size_t source_bytes = literal.bytes.size();
	const unsigned source_width = static_cast<unsigned>(min<size_t>(
		source_bytes, sizeof(uint64_t)) * 8);
	uint64_t value = ReadLiteral(literal);
	if (source_width != 0 && source_width < 64)
		value &= WidthMask(source_width);
	if (negated)
	{
		if (source_width == 0)
			value = 0;
		else
			value = (uint64_t(0) - value) & WidthMask(source_width);
	}
	if (width > source_width && source_width != 0 &&
		Cy86IsSignedIntegral(literal.type) &&
		(value & (uint64_t(1) << (source_width - 1))))
		value |= ~WidthMask(source_width);
	return value & WidthMask(width);
}

uint64_t ResolveLabel(const map<string, uint64_t>& labels,
	const string& name)
{
	map<string, uint64_t>::const_iterator it = labels.find(name);
	if (it == labels.end())
		throw Cy86Error("undefined CY86 label: " + name);
	return it->second;
}

uint64_t ImmediateValue(const Cy86Immediate& immediate, unsigned width,
	const map<string, uint64_t>& labels)
{
	if (!immediate.label)
		return ConvertLiteral(immediate.literal, width, immediate.negated);
	uint64_t value = ResolveLabel(labels, immediate.label_name);
	if (immediate.has_addend)
	{
		const uint64_t addend = ConvertLiteral(immediate.addend, 64, false);
		value = immediate.addend_negative ? value - addend : value + addend;
	}
	return value & WidthMask(width);
}

unsigned OpcodeWidth(const string& opcode)
{
	size_t first_digit = opcode.find_first_of("0123456789");
	if (first_digit == string::npos)
		throw Cy86Error("opcode has no width: " + opcode);
	unsigned width = 0;
	for (size_t i = first_digit; i < opcode.size(); ++i)
	{
		if (opcode[i] < '0' || opcode[i] > '9')
			throw Cy86Error("invalid opcode width: " + opcode);
		width = width * 10 + static_cast<unsigned>(opcode[i] - '0');
	}
	return width;
}

bool StartsWith(const string& value, const string& prefix)
{
	return value.compare(0, prefix.size(), prefix) == 0;
}

bool CanUseBinaryImmediate(unsigned width, uint64_t value)
{
	if (width < 64)
		return true;
	return value <= static_cast<uint64_t>(numeric_limits<int32_t>::max()) ||
		value >= uint64_t(0xffffffff80000000ULL);
}

void EmitMoveImmediate(ByteVector& output, X64Register reg, uint64_t value)
{
	Emit(output, X86_MOV, 64, Two(X86Reg(reg, 64), X86Imm(value, 64)));
}

uint64_t LiteralChunk(const Cy86Literal& literal, size_t offset,
	size_t count)
{
	uint64_t value = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const size_t index = offset + i;
		if (index < literal.bytes.size())
			value |= uint64_t(literal.bytes[index]) << (i * 8);
	}
	return value;
}

void WriteLiteralToBounce(ByteVector& output, const Cy86Literal& literal,
	size_t byte_count, int64_t offset)
{
	size_t position = 0;
	while (byte_count - position >= 8)
	{
		EmitMoveImmediate(output, XR_RAX,
			LiteralChunk(literal, position, 8));
		Emit(output, X86_MOV, 64,
			Two(X86Mem(XR_RSP, offset + static_cast<int64_t>(position), 64),
				X86Reg(XR_RAX, 64)));
		position += 8;
	}
	if (byte_count - position >= 4)
	{
		Emit(output, X86_MOV, 32,
			Two(X86Reg(XR_RAX, 32),
				X86Imm(LiteralChunk(literal, position, 4), 32)));
		Emit(output, X86_MOV, 32,
			Two(X86Mem(XR_RSP, offset + static_cast<int64_t>(position), 32),
				X86Reg(XR_RAX, 32)));
		position += 4;
	}
	if (byte_count - position >= 2)
	{
		Emit(output, X86_MOV, 16,
			Two(X86Reg(XR_RAX, 16),
				X86Imm(LiteralChunk(literal, position, 2), 16)));
		Emit(output, X86_MOV, 16,
			Two(X86Mem(XR_RSP, offset + static_cast<int64_t>(position), 16),
				X86Reg(XR_RAX, 16)));
		position += 2;
	}
	if (byte_count != position)
	{
		Emit(output, X86_MOV, 8,
			Two(X86Reg(XR_RAX, 8),
				X86Imm(LiteralChunk(literal, position, 1), 8)));
		Emit(output, X86_MOV, 8,
			Two(X86Mem(XR_RSP, offset + static_cast<int64_t>(position), 8),
				X86Reg(XR_RAX, 8)));
	}
}

void StoreRawRegister(ByteVector& output, X64Register source,
	unsigned width, int64_t offset)
{
	Emit(output, X86_MOV, width,
		Two(X86Mem(XR_RSP, offset, width), X86Reg(source, width)));
}

void LoadRawBounce(ByteVector& output, X64Register target, unsigned width,
	int64_t offset)
{
	Emit(output, X86_MOV, width,
		Two(X86Reg(target, width), X86Mem(XR_RSP, offset, width)));
}

uint64_t MemoryBase(const Cy86Memory& memory,
	const map<string, uint64_t>& labels)
{
	if (memory.base_is_label)
		return ResolveLabel(labels, memory.base_label);
	if (memory.base_is_literal)
		return ConvertLiteral(memory.base_literal, 64, false);
	throw Cy86Error("memory operand has no absolute base");
}

void EmitAddress(ByteVector& output, const Cy86Memory& memory,
	const map<string, uint64_t>& labels, X64Register address_reg)
{
	if (memory.base_is_register)
		Emit(output, X86_MOV, 64,
			Two(X86Reg(address_reg, 64),
				X86Reg(memory.base_register.reg, 64)));
	else
		EmitMoveImmediate(output, address_reg, MemoryBase(memory, labels));
	if (memory.has_offset)
	{
		uint64_t offset = ConvertLiteral(memory.offset, 64, false);
		if (memory.offset_negative)
			offset = uint64_t(0) - offset;
		EmitMoveImmediate(output, XR_RBX, offset);
		Emit(output, X86_ADD, 64,
			Two(X86Reg(address_reg, 64), X86Reg(XR_RBX, 64)));
	}
}

void LoadOperand(ByteVector& output, const Cy86Operand& operand,
	unsigned width, X64Register target,
	const map<string, uint64_t>& labels)
{
	switch (operand.kind)
	{
	case CY86_REGISTER_OPERAND:
		Emit(output, X86_MOV, width,
			Two(X86Reg(target, width), X86Reg(operand.reg.reg, width)));
		return;
	case CY86_IMMEDIATE_OPERAND:
		Emit(output, X86_MOV, width,
			Two(X86Reg(target, width),
				X86Imm(ImmediateValue(operand.immediate, width, labels), width)));
		return;
	case CY86_MEMORY_OPERAND:
		EmitAddress(output, operand.memory, labels, XR_RSI);
		Emit(output, X86_MOV, width,
			Two(X86Reg(target, width), X86Mem(XR_RSI, 0, width)));
		return;
	}
	throw Cy86Error("invalid CY86 operand kind");
}

void StoreOperand(ByteVector& output, const Cy86Operand& operand,
	unsigned width, X64Register source,
	const map<string, uint64_t>& labels)
{
	if (operand.kind == CY86_REGISTER_OPERAND)
	{
		Emit(output, X86_MOV, width,
			Two(X86Reg(operand.reg.reg, width), X86Reg(source, width)));
		return;
	}
	if (operand.kind == CY86_MEMORY_OPERAND)
	{
		EmitAddress(output, operand.memory, labels, XR_RDI);
		Emit(output, X86_MOV, width,
			Two(X86Mem(XR_RDI, 0, width), X86Reg(source, width)));
		return;
	}
	throw Cy86Error("write operand is not writable");
}

unsigned FloatLiteralWidth(const Cy86Literal& literal, unsigned fallback)
{
	switch (literal.type)
	{
	case FT_FLOAT: return 32;
	case FT_DOUBLE: return 64;
	case FT_LONG_DOUBLE: return 80;
	default: return fallback;
	}
}

void LoadFloatOperand(ByteVector& output, const Cy86Operand& operand,
	unsigned width, const map<string, uint64_t>& labels, int64_t bounce)
{
	if (operand.kind == CY86_MEMORY_OPERAND)
	{
		EmitAddress(output, operand.memory, labels, XR_RSI);
		Emit(output, X86_FLD, width,
			One(X86Mem(XR_RSI, 0, width)));
		return;
	}
	if (operand.kind != CY86_IMMEDIATE_OPERAND)
		throw Cy86Error("floating register operand cannot be lowered");
	const unsigned literal_width = FloatLiteralWidth(operand.immediate.literal,
		width);
	const size_t byte_count = literal_width == 80 ? 10 : literal_width / 8;
	WriteLiteralToBounce(output, operand.immediate.literal, byte_count, bounce);
	Emit(output, X86_FLD, literal_width,
		One(X86Mem(XR_RSP, bounce, literal_width)));
}

void StoreFloatOperand(ByteVector& output, const Cy86Operand& operand,
	unsigned width, const map<string, uint64_t>& labels)
{
	if (operand.kind != CY86_MEMORY_OPERAND)
		throw Cy86Error("floating result must be stored in memory");
	EmitAddress(output, operand.memory, labels, XR_RDI);
	Emit(output, X86_FSTP, width,
		One(X86Mem(XR_RDI, 0, width)));
}

void TranslateMove(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
}

void TranslateMove80(ByteVector& output, const Cy86Statement& statement,
	const map<string, uint64_t>& labels)
{
	const Cy86Operand& source = statement.operands[1];
	if (source.kind == CY86_IMMEDIATE_OPERAND)
		WriteLiteralToBounce(output, source.immediate.literal, 10,
			kBouncePrimary);
	else if (source.kind == CY86_MEMORY_OPERAND)
	{
		EmitAddress(output, source.memory, labels, XR_RSI);
		Emit(output, X86_MOV, 64,
			Two(X86Reg(XR_RAX, 64), X86Mem(XR_RSI, 0, 64)));
		StoreRawRegister(output, XR_RAX, 64, kBouncePrimary);
		Emit(output, X86_MOV, 16,
			Two(X86Reg(XR_RAX, 16), X86Mem(XR_RSI, 8, 16)));
		StoreRawRegister(output, XR_RAX, 16, kBouncePrimary + 8);
	}
	else
		throw Cy86Error("move80 source is not readable");
	if (statement.operands[0].kind != CY86_MEMORY_OPERAND)
		throw Cy86Error("move80 destination is not writable memory");
	EmitAddress(output, statement.operands[0].memory, labels, XR_RDI);
	LoadRawBounce(output, XR_RAX, 64, kBouncePrimary);
	Emit(output, X86_MOV, 64,
		Two(X86Mem(XR_RDI, 0, 64), X86Reg(XR_RAX, 64)));
	LoadRawBounce(output, XR_RAX, 16, kBouncePrimary + 8);
	Emit(output, X86_MOV, 16,
		Two(X86Mem(XR_RDI, 8, 16), X86Reg(XR_RAX, 16)));
}

X86Mnemonic BinaryMnemonic(const string& opcode)
{
	if (StartsWith(opcode, "iadd")) return X86_ADD;
	if (StartsWith(opcode, "isub")) return X86_SUB;
	if (StartsWith(opcode, "and")) return X86_AND;
	if (StartsWith(opcode, "or")) return X86_OR;
	if (StartsWith(opcode, "xor")) return X86_XOR;
	throw Cy86Error("not an integer binary opcode: " + opcode);
}

void TranslateBinary(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	if (statement.operands[2].kind == CY86_IMMEDIATE_OPERAND &&
		!statement.operands[2].immediate.label)
	{
		const uint64_t value = ImmediateValue(statement.operands[2].immediate,
			width, labels);
		if (CanUseBinaryImmediate(width, value))
		{
			Emit(output, BinaryMnemonic(statement.opcode), width,
				Two(X86Reg(XR_RAX, width), X86Imm(value, width)));
			StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
			return;
		}
	}
	LoadOperand(output, statement.operands[2], width, XR_RBX, labels);
	Emit(output, BinaryMnemonic(statement.opcode), width,
		Two(X86Reg(XR_RAX, width), X86Reg(XR_RBX, width)));
	StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
}

X86Condition CompareCondition(const string& opcode)
{
	if (StartsWith(opcode, "ieq")) return XC_E;
	if (StartsWith(opcode, "ine")) return XC_NE;
	if (StartsWith(opcode, "slt")) return XC_L;
	if (StartsWith(opcode, "ult")) return XC_B;
	if (StartsWith(opcode, "sgt")) return XC_G;
	if (StartsWith(opcode, "ugt")) return XC_A;
	if (StartsWith(opcode, "sle")) return XC_LE;
	if (StartsWith(opcode, "ule")) return XC_BE;
	if (StartsWith(opcode, "sge")) return XC_GE;
	if (StartsWith(opcode, "uge")) return XC_AE;
	throw Cy86Error("not an integer comparison opcode: " + opcode);
}

void TranslateCompare(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	if (statement.operands[2].kind == CY86_IMMEDIATE_OPERAND &&
		!statement.operands[2].immediate.label)
	{
		const uint64_t value = ImmediateValue(statement.operands[2].immediate,
			width, labels);
		if (CanUseBinaryImmediate(width, value))
		{
			Emit(output, X86_CMP, width,
				Two(X86Reg(XR_RAX, width), X86Imm(value, width)));
			Emit(output, X86_SETCC, 8, CompareCondition(statement.opcode),
				One(X86Reg(XR_RAX, 8)));
			StoreOperand(output, statement.operands[0], 8, XR_RAX, labels);
			return;
		}
	}
	LoadOperand(output, statement.operands[2], width, XR_RBX, labels);
	Emit(output, X86_CMP, width,
			Two(X86Reg(XR_RAX, width), X86Reg(XR_RBX, width)));
	Emit(output, X86_SETCC, 8, CompareCondition(statement.opcode),
		One(X86Reg(XR_RAX, 8)));
	StoreOperand(output, statement.operands[0], 8, XR_RAX, labels);
}

void TranslateShift(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	LoadOperand(output, statement.operands[2], 8, XR_RCX, labels);
	const X86Mnemonic mnemonic = StartsWith(statement.opcode, "lshift") ?
		X86_SHL : (StartsWith(statement.opcode, "srshift") ? X86_SAR : X86_SHR);
	Emit(output, mnemonic, width,
		Two(X86Reg(XR_RAX, width), X86Reg(XR_RCX, 8)));
	StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
}

void TranslateJump(ByteVector& output, const Cy86Statement& statement,
	const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[0], 64, XR_RBX, labels);
	Emit(output, statement.opcode == "call" ? X86_CALL : X86_JMP, 64,
		One(X86Reg(XR_RBX, 64)));
}

void TranslateJumpIf(ByteVector& output, const Cy86Statement& statement,
	const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[0], 8, XR_RAX, labels);
	LoadOperand(output, statement.operands[1], 64, XR_RBX, labels);
	Emit(output, X86_TEST, 8,
		Two(X86Reg(XR_RAX, 8), X86Reg(XR_RAX, 8)));
	Emit(output, X86_JCC_REL8, 8, XC_E, One(X86Rel(2)));
	Emit(output, X86_JMP, 64, One(X86Reg(XR_RBX, 64)));
}

const X64Register kSyscallArguments[] =
	{XR_RDI, XR_RSI, XR_RDX, XR_R10, XR_R8, XR_R9};

unsigned ParseSyscallNumber(const string& opcode)
{
	if (!StartsWith(opcode, "syscall"))
		throw Cy86Error("not a syscall opcode");
	unsigned count = 0;
	for (size_t i = 7; i < opcode.size(); ++i)
	{
		if (opcode[i] < '0' || opcode[i] > '9')
			throw Cy86Error("invalid syscall opcode");
		count = count * 10 + static_cast<unsigned>(opcode[i] - '0');
	}
	return count;
}

void TranslateSyscall(ByteVector& output, const Cy86Statement& statement,
	const map<string, uint64_t>& labels)
{
	const unsigned count = ParseSyscallNumber(statement.opcode);
	if (count > 6 || statement.operands.size() != count + 2)
		throw Cy86Error("invalid syscall operand count");
	LoadOperand(output, statement.operands[1], 64, XR_RBX, labels);
	for (int i = static_cast<int>(count) - 1; i >= 0; --i)
	{
		LoadOperand(output, statement.operands[2 + i], 64, XR_RAX, labels);
		Emit(output, X86_MOV, 64,
			Two(X86Reg(kSyscallArguments[i], 64), X86Reg(XR_RAX, 64)));
	}
	Emit(output, X86_MOV, 64,
		Two(X86Reg(XR_RAX, 64), X86Reg(XR_RBX, 64)));
	Emit(output, X86_SYSCALL, 0, vector<X86Operand>());
	StoreOperand(output, statement.operands[0], 64, XR_RAX, labels);
}

void TranslateUnary(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	Emit(output, X86_NOT, width, One(X86Reg(XR_RAX, width)));
	StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
}

void ClearHighHalf(ByteVector& output, unsigned width)
{
	const unsigned clear_width = width == 16 ? 16 : 32;
	Emit(output, X86_XOR, clear_width,
		Two(X86Reg(XR_RDX, clear_width), X86Reg(XR_RDX, clear_width)));
}

void PrepareDivideDividend(ByteVector& output, unsigned width,
	bool signed_op)
{
	if (width == 8)
	{
		if (signed_op)
			Emit(output, X86_CBW, 0, vector<X86Operand>());
		else
			Emit(output, X86_MOV, 8,
				Two(X86HighByte(XR_RAX), X86Imm(0, 8)));
		return;
	}
	if (signed_op)
	{
		if (width == 16)
			Emit(output, X86_CWD, 0, vector<X86Operand>());
		else if (width == 32)
			Emit(output, X86_CDQ, 0, vector<X86Operand>());
		else
			Emit(output, X86_CQO, 0, vector<X86Operand>());
	}
	else
		ClearHighHalf(output, width);
}

void TranslateMultiply(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	LoadOperand(output, statement.operands[2], width, XR_RBX, labels);
	const bool signed_op = StartsWith(statement.opcode, "smul");
	Emit(output, signed_op ? X86_IMUL : X86_MUL, width,
		One(X86Reg(XR_RBX, width)));
	StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
}

void TranslateDivide(ByteVector& output, const Cy86Statement& statement,
	unsigned width, const map<string, uint64_t>& labels)
{
	const bool signed_op = StartsWith(statement.opcode, "sdiv") ||
		StartsWith(statement.opcode, "smod");
	const bool remainder = StartsWith(statement.opcode, "smod") ||
		StartsWith(statement.opcode, "umod");
	LoadOperand(output, statement.operands[1], width, XR_RAX, labels);
	LoadOperand(output, statement.operands[2], width, XR_RBX, labels);
	PrepareDivideDividend(output, width, signed_op);
	Emit(output, signed_op ? X86_IDIV : X86_DIV, width,
		One(X86Reg(XR_RBX, width)));
	if (remainder && width == 8)
	{
		Emit(output, X86_MOV, 8,
			Two(X86Reg(XR_RDX, 8), X86HighByte(XR_RAX)));
		StoreOperand(output, statement.operands[0], 8, XR_RDX, labels);
	}
	else
		StoreOperand(output, statement.operands[0], width,
			remainder ? XR_RDX : XR_RAX, labels);
}

unsigned ConversionWidth(const string& opcode)
{
	if (StartsWith(opcode, "s8") || StartsWith(opcode, "u8") ||
		StartsWith(opcode, "f80convs8") ||
		StartsWith(opcode, "f80convu8"))
		return 8;
	if (StartsWith(opcode, "s16") || StartsWith(opcode, "u16") ||
		StartsWith(opcode, "f80convs16") ||
		StartsWith(opcode, "f80convu16"))
		return 16;
	if (StartsWith(opcode, "f32convf80"))
		return 32;
	if (StartsWith(opcode, "f64convf80"))
		return 64;
	if (StartsWith(opcode, "f80convf32"))
		return 32;
	if (StartsWith(opcode, "f80convf64"))
		return 64;
	if (StartsWith(opcode, "s32") || StartsWith(opcode, "u32"))
		return 32;
	if (StartsWith(opcode, "s64") || StartsWith(opcode, "u64"))
		return 64;
	if (StartsWith(opcode, "f80convs32") ||
		StartsWith(opcode, "f80convu32"))
		return 32;
	if (StartsWith(opcode, "f80convs64") ||
		StartsWith(opcode, "f80convu64"))
		return 64;
	throw Cy86Error("unknown CY86 conversion width: " + opcode);
}

void TranslateIntegerToFloat80(ByteVector& output,
	const Cy86Statement& statement, unsigned width, bool signed_op,
	const map<string, uint64_t>& labels)
{
	const Cy86Operand& source = statement.operands[1];
	if (width == 8)
	{
		if (!signed_op)
			Emit(output, X86_XOR, 32,
				Two(X86Reg(XR_RAX, 32), X86Reg(XR_RAX, 32)));
		LoadOperand(output, source, 8, XR_RAX, labels);
		if (signed_op)
			Emit(output, X86_CBW, 0, vector<X86Operand>());
		StoreRawRegister(output, XR_RAX, 16, kBouncePrimary);
		Emit(output, X86_FILD, 16,
			One(X86Mem(XR_RSP, kBouncePrimary, 16)));
	}
	else if (width == 16)
	{
		if (signed_op)
		{
			LoadOperand(output, source, 16, XR_RAX, labels);
			StoreRawRegister(output, XR_RAX, 16, kBouncePrimary);
			Emit(output, X86_FILD, 16,
				One(X86Mem(XR_RSP, kBouncePrimary, 16)));
		}
		else
		{
			Emit(output, X86_XOR, 32,
				Two(X86Reg(XR_RAX, 32), X86Reg(XR_RAX, 32)));
			LoadOperand(output, source, 16, XR_RAX, labels);
			StoreRawRegister(output, XR_RAX, 32, kBouncePrimary);
			Emit(output, X86_FILD, 32,
				One(X86Mem(XR_RSP, kBouncePrimary, 32)));
		}
	}
	else if (width == 32)
	{
		LoadOperand(output, source, 32, XR_RAX, labels);
		if (signed_op)
		{
			StoreRawRegister(output, XR_RAX, 32, kBouncePrimary);
			Emit(output, X86_FILD, 32,
				One(X86Mem(XR_RSP, kBouncePrimary, 32)));
		}
		else
		{
			// A zero-extending 32-bit load makes the value fit in a
			// signed 64-bit FILD operand.
			StoreRawRegister(output, XR_RAX, 64, kBouncePrimary);
			Emit(output, X86_FILD, 64,
				One(X86Mem(XR_RSP, kBouncePrimary, 64)));
		}
	}
	else if (signed_op)
	{
		LoadOperand(output, source, 64, XR_RAX, labels);
		StoreRawRegister(output, XR_RAX, 64, kBouncePrimary);
		Emit(output, X86_FILD, 64,
			One(X86Mem(XR_RSP, kBouncePrimary, 64)));
	}
	else
	{
		// Convert n as 2 * (n >> 1) + (n & 1).  Both FILD operands
		// are signed, while the intermediate values stay in range.
		LoadOperand(output, source, 64, XR_RAX, labels);
		Emit(output, X86_MOV, 64,
			Two(X86Reg(XR_RBX, 64), X86Reg(XR_RAX, 64)));
		Emit(output, X86_AND, 64,
			Two(X86Reg(XR_RAX, 64), X86Imm(1, 64)));
		StoreRawRegister(output, XR_RAX, 16, kBouncePrimary + 8);
		EmitMoveImmediate(output, XR_RCX, 1);
		Emit(output, X86_SHR, 64,
			Two(X86Reg(XR_RBX, 64), X86Reg(XR_RCX, 8)));
		StoreRawRegister(output, XR_RBX, 64, kBouncePrimary);
		Emit(output, X86_FILD, 64,
			One(X86Mem(XR_RSP, kBouncePrimary, 64)));
		Emit(output, X86_FLD, 80, One(X87Reg(0)));
		Emit(output, X86_FADDP, 0, One(X87Reg(1)));
		Emit(output, X86_FILD, 16,
			One(X86Mem(XR_RSP, kBouncePrimary + 8, 16)));
		Emit(output, X86_FADDP, 0, One(X87Reg(1)));
	}
	StoreFloatOperand(output, statement.operands[0], 80, labels);
}

void TranslateFloatToFloat80(ByteVector& output,
	const Cy86Statement& statement, unsigned width,
	const map<string, uint64_t>& labels)
{
	LoadFloatOperand(output, statement.operands[1], width, labels,
		kBouncePrimary);
	StoreFloatOperand(output, statement.operands[0], 80, labels);
}

void TranslateFloatFrom80(ByteVector& output,
	const Cy86Statement& statement, unsigned width, bool unsigned_op,
	bool floating_op,
	const map<string, uint64_t>& labels)
{
	LoadFloatOperand(output, statement.operands[1], 80, labels,
		kBouncePrimary);
	if (floating_op)
	{
		StoreFloatOperand(output, statement.operands[0], width, labels);
		return;
	}
	if (!unsigned_op && (width == 32 || width == 64))
	{
		Emit(output, X86_FISTP, width,
			One(X86Mem(XR_RSP, kBounceSecondary, width)));
		LoadRawBounce(output, XR_RAX, width, kBounceSecondary);
		StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
		return;
	}
	if (unsigned_op && width == 64)
	{
		// Subtract 2^63, convert the signed result, then toggle the
		// high bit.  This is valid for every exactly representable
		// uint64 value and avoids an out-of-range signed FISTP.
		EmitMoveImmediate(output, XR_RAX, uint64_t(0x8000000000000000ULL));
		StoreRawRegister(output, XR_RAX, 64, kBounceSecondary);
		Emit(output, X86_FILD, 64,
			One(X86Mem(XR_RSP, kBounceSecondary, 64)));
		Emit(output, X86_FCHS, 0, vector<X86Operand>());
		Emit(output, X86_FSUBP, 0, One(X87Reg(1)));
		Emit(output, X86_FISTP, 64,
			One(X86Mem(XR_RSP, kBounceTertiary, 64)));
		LoadRawBounce(output, XR_RAX, 64, kBounceTertiary);
		EmitMoveImmediate(output, XR_RBX, uint64_t(0x8000000000000000ULL));
		Emit(output, X86_XOR, 64,
			Two(X86Reg(XR_RAX, 64), X86Reg(XR_RBX, 64)));
		StoreOperand(output, statement.operands[0], 64, XR_RAX, labels);
		return;
	}
	const unsigned storage_width = width == 8 ? 16 :
		(unsigned_op ? (width == 16 ? 32 : 64) : width);
	Emit(output, X86_FISTP, storage_width,
		One(X86Mem(XR_RSP, kBounceSecondary, storage_width)));
	LoadRawBounce(output, XR_RAX, storage_width, kBounceSecondary);
	StoreOperand(output, statement.operands[0], width, XR_RAX, labels);
}

X86Mnemonic FloatBinaryMnemonic(const string& opcode)
{
	if (StartsWith(opcode, "fadd")) return X86_FADDP;
	if (StartsWith(opcode, "fsub")) return X86_FSUBP;
	if (StartsWith(opcode, "fmul")) return X86_FMULP;
	if (StartsWith(opcode, "fdiv")) return X86_FDIVP;
	throw Cy86Error("not a floating binary opcode: " + opcode);
}

void TranslateFloatBinary(ByteVector& output,
	const Cy86Statement& statement, unsigned width,
	const map<string, uint64_t>& labels)
{
	LoadFloatOperand(output, statement.operands[1], width, labels,
		kBouncePrimary);
	LoadFloatOperand(output, statement.operands[2], width, labels,
		kBounceSecondary);
	Emit(output, FloatBinaryMnemonic(statement.opcode), 0,
		One(X87Reg(1)));
	StoreFloatOperand(output, statement.operands[0], width, labels);
}

X86Condition FloatCompareCondition(const string& opcode)
{
	if (StartsWith(opcode, "feq")) return XC_E;
	if (StartsWith(opcode, "fne")) return XC_NE;
	if (StartsWith(opcode, "flt")) return XC_B;
	if (StartsWith(opcode, "fgt")) return XC_A;
	if (StartsWith(opcode, "fle")) return XC_BE;
	if (StartsWith(opcode, "fge")) return XC_AE;
	throw Cy86Error("not a floating comparison opcode: " + opcode);
}

void TranslateFloatCompare(ByteVector& output,
	const Cy86Statement& statement, unsigned width,
	const map<string, uint64_t>& labels)
{
	// FCOMIP compares ST(0) against ST(i); load the right operand
	// first so the resulting flags have the CY86 left/right order.
	LoadFloatOperand(output, statement.operands[2], width, labels,
		kBouncePrimary);
	LoadFloatOperand(output, statement.operands[1], width, labels,
		kBounceSecondary);
	Emit(output, X86_FCOMIP, 0, One(X87Reg(1)));
	Emit(output, X86_FSTP, 0, One(X87Reg(0)));
	Emit(output, X86_SETCC, 8, FloatCompareCondition(statement.opcode),
		One(X86Reg(XR_RAX, 8)));
	StoreOperand(output, statement.operands[0], 8, XR_RAX, labels);
}

ByteVector MaterializeData(const Cy86Statement& statement)
{
	const size_t width = statement.data_width;
	if (width == 0)
	{
		ByteVector result = statement.literal.bytes;
		if (!statement.negated)
			return result;
		if (!Cy86IsArithmetic(statement.literal.type) ||
			statement.literal.is_array)
			throw Cy86Error("cannot negate CY86 data literal");
		for (size_t i = 0; i < result.size(); ++i)
			result[i] = static_cast<unsigned char>(~result[i]);
		for (size_t i = 0; i < result.size(); ++i)
		{
			++result[i];
			if (result[i] != 0)
				break;
		}
		return result;
	}
	if (width <= 8)
	{
		const uint64_t value = ConvertLiteral(statement.literal,
			static_cast<unsigned>(width * 8), statement.negated);
		ByteVector result(width);
		for (size_t i = 0; i < width; ++i)
			result[i] = static_cast<unsigned char>(value >> (i * 8));
		return result;
	}
	ByteVector result(width, 0);
	const size_t count = min(width, statement.literal.bytes.size());
	copy(statement.literal.bytes.begin(), statement.literal.bytes.begin() + count,
		result.begin());
	return result;
}

size_t DataAlignment(const Cy86Statement& statement)
{
	if (statement.data_width != 0)
		return statement.data_width;
	if (statement.literal.is_array || statement.literal.num_elements > 1)
	{
		if (statement.literal.num_elements == 0)
			throw Cy86Error("array literal has no elements");
		return max<size_t>(1, statement.literal.bytes.size() /
			statement.literal.num_elements);
	}
	if (statement.literal.type == FT_LONG_DOUBLE)
		return 16;
	return max<size_t>(1, Cy86FundamentalSize(statement.literal.type));
}

size_t AlignUp(size_t value, size_t alignment)
{
	if (alignment == 0)
		throw Cy86Error("zero CY86 alignment");
	const size_t remainder = value % alignment;
	return remainder == 0 ? value : value + alignment - remainder;
}

struct ImageElfHeader
{
	unsigned char ident[16] =
	{
		0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	short int type = 2;
	short int machine = 0x3E;
	int version = 1;
	long int entry;
	long int phoff = 64;
	long int shoff = 0;
	int processor_flags = 0;
	short int ehsize = 64;
	short int phentsize = 56;
	short int phnum = 1;
	short int shentsize = 0;
	short int shnum = 0;
	short int shstrndx = 0;
};

struct ImageProgramSegmentHeader
{
	int type = 1;
	static constexpr int executable = 1 << 0;
	static constexpr int writable = 1 << 1;
	static constexpr int readable = 1 << 2;
	int flags = executable | writable | readable;
	long int offset = 0;
	long int vaddr = 0x400000;
	long int paddr = 0;
	long int filesz;
	long int memsz;
	long int align = 0;
};

static_assert(sizeof(ImageElfHeader) == 64, "unexpected ELF header layout");
static_assert(sizeof(ImageProgramSegmentHeader) == 56,
	"unexpected program header layout");

} // namespace

vector<unsigned char> Cy86ToX86Translator::Translate(const Cy86Statement& statement,
	const map<string, uint64_t>& labels) const
{
	if (statement.is_data)
		throw Cy86Error("data statement has no instruction encoding");
	ByteVector output;
	const string& opcode = statement.opcode;
	if (opcode == "move80")
		TranslateMove80(output, statement, labels);
	else if (StartsWith(opcode, "move"))
		TranslateMove(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "f80conv"))
		TranslateFloatFrom80(output, statement, ConversionWidth(opcode),
			StartsWith(opcode, "f80convu"), StartsWith(opcode, "f80convf"),
			labels);
	else if (opcode.find("convf80") != string::npos &&
		StartsWith(opcode, "f32"))
		TranslateFloatToFloat80(output, statement, ConversionWidth(opcode),
			labels);
	else if (opcode.find("convf80") != string::npos &&
		StartsWith(opcode, "f64"))
		TranslateFloatToFloat80(output, statement, ConversionWidth(opcode),
			labels);
	else if (opcode.find("convf80") != string::npos &&
		(StartsWith(opcode, "s") || StartsWith(opcode, "u")))
		TranslateIntegerToFloat80(output, statement, ConversionWidth(opcode),
			StartsWith(opcode, "s"), labels);
	else if (StartsWith(opcode, "fadd") || StartsWith(opcode, "fsub") ||
		StartsWith(opcode, "fmul") || StartsWith(opcode, "fdiv"))
		TranslateFloatBinary(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "feq") || StartsWith(opcode, "fne") ||
		StartsWith(opcode, "flt") || StartsWith(opcode, "fgt") ||
		StartsWith(opcode, "fle") || StartsWith(opcode, "fge"))
		TranslateFloatCompare(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "iadd") || StartsWith(opcode, "isub") ||
		StartsWith(opcode, "and") || StartsWith(opcode, "or") ||
		StartsWith(opcode, "xor"))
		TranslateBinary(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "ieq") || StartsWith(opcode, "ine") ||
		StartsWith(opcode, "slt") || StartsWith(opcode, "ult") ||
		StartsWith(opcode, "sgt") || StartsWith(opcode, "ugt") ||
		StartsWith(opcode, "sle") || StartsWith(opcode, "ule") ||
		StartsWith(opcode, "sge") || StartsWith(opcode, "uge"))
		TranslateCompare(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "lshift") || StartsWith(opcode, "srshift") ||
		StartsWith(opcode, "urshift"))
		TranslateShift(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "not"))
		TranslateUnary(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "smul") || StartsWith(opcode, "umul"))
		TranslateMultiply(output, statement, OpcodeWidth(opcode), labels);
	else if (StartsWith(opcode, "sdiv") || StartsWith(opcode, "udiv") ||
		StartsWith(opcode, "smod") || StartsWith(opcode, "umod"))
		TranslateDivide(output, statement, OpcodeWidth(opcode), labels);
	else if (opcode == "jump" || opcode == "call")
		TranslateJump(output, statement, labels);
	else if (opcode == "jumpif")
		TranslateJumpIf(output, statement, labels);
	else if (StartsWith(opcode, "syscall"))
		TranslateSyscall(output, statement, labels);
	else if (opcode == "ret")
		Emit(output, X86_RET, 0, vector<X86Operand>());
	else
		throw Cy86Error("opcode is outside the integer checkpoint: " + opcode);
	return output;
}

vector<unsigned char> Cy86ToX86Translator::Stub(uint64_t entry)
{
	ByteVector output;
	const X64Register zeroed[] = {XR_R12, XR_R13, XR_R14, XR_R15};
	for (size_t i = 0; i < sizeof(zeroed) / sizeof(zeroed[0]); ++i)
		Emit(output, X86_XOR, 32,
			Two(X86Reg(zeroed[i], 32), X86Reg(zeroed[i], 32)));
	Emit(output, X86_MOV, 64,
		Two(X86Reg(XR_RBP, 64), X86Reg(XR_RSP, 64)));
	Emit(output, X86_MOV, 64,
		Two(X86Reg(XR_RAX, 64), X86ImmFullWidth(entry, 64)));
	Emit(output, X86_JMP, 64, One(X86Reg(XR_RAX, 64)));
	if (output.size() != 27)
		throw Cy86Error("CY86 bootstrap size changed");
	return output;
}

vector<unsigned char> Cy86ToX86Translator::Epilogue()
{
	ByteVector output;
	EmitMoveImmediate(output, XR_RAX, 60);
	EmitMoveImmediate(output, XR_RDI, 0);
	Emit(output, X86_SYSCALL, 0, vector<X86Operand>());
	Emit(output, X86_UD2, 0, vector<X86Operand>());
	return output;
}

Cy86Layout BuildCy86Layout(const vector<Cy86Statement>& statements)
{
	Cy86Layout layout;
	map<string, uint64_t> placeholders;
	for (size_t i = 0; i < statements.size(); ++i)
		for (size_t j = 0; j < statements[i].labels.size(); ++j)
			placeholders[statements[i].labels[j]] = 0;
	const size_t stub_size = Cy86ToX86Translator::Stub(0).size();
	size_t current = kImageHeaderSize + stub_size;
	Cy86ToX86Translator translator;
	layout.statement_offsets.resize(statements.size());
	layout.statement_sizes.resize(statements.size());
	for (size_t i = 0; i < statements.size(); ++i)
	{
		if (statements[i].is_data)
			current = AlignUp(current, DataAlignment(statements[i]));
		layout.statement_offsets[i] = current;
		for (size_t j = 0; j < statements[i].labels.size(); ++j)
			layout.labels[statements[i].labels[j]] = kImageBase + current;
		if (statements[i].is_data)
		{
			layout.statement_sizes[i] = MaterializeData(statements[i]).size();
			current += layout.statement_sizes[i];
		}
		else
		{
			layout.statement_sizes[i] =
				translator.Translate(statements[i], placeholders).size();
			current += layout.statement_sizes[i];
		}
	}
	layout.epilogue_offset = current;
	layout.entry = kImageBase + current;
	map<string, uint64_t>::const_iterator start = layout.labels.find("start");
	if (start != layout.labels.end())
		layout.entry = start->second;
	else if (!statements.empty())
		layout.entry = kImageBase + layout.statement_offsets[0];
	layout.body_size = current + Cy86ToX86Translator::Epilogue().size() -
		kImageHeaderSize;
	return layout;
}

vector<unsigned char> BuildProgramImage(const vector<Cy86Statement>& statements)
{
	const Cy86Layout layout = BuildCy86Layout(statements);
	const Cy86ToX86Translator translator;
	ByteVector file(kImageHeaderSize, 0);
	Append(file, Cy86ToX86Translator::Stub(layout.entry));
	size_t current = file.size();
	for (size_t i = 0; i < statements.size(); ++i)
	{
		while (current < layout.statement_offsets[i])
		{
			file.push_back(0);
			++current;
		}
		ByteVector bytes;
		if (statements[i].is_data)
			bytes = MaterializeData(statements[i]);
		else
			bytes = translator.Translate(statements[i], layout.labels);
		if (bytes.size() != layout.statement_sizes[i])
			throw Cy86Error("CY86 pass sizes diverged");
		Append(file, bytes);
		current += bytes.size();
	}
	while (current < layout.epilogue_offset)
	{
		file.push_back(0);
		++current;
	}
	Append(file, Cy86ToX86Translator::Epilogue());
	if (file.size() != kImageHeaderSize + layout.body_size)
		throw Cy86Error("CY86 image layout size mismatch");

	ImageElfHeader header;
	header.entry = static_cast<long int>(kImageBase + kImageHeaderSize);
	ImageProgramSegmentHeader segment;
	segment.filesz = static_cast<long int>(file.size());
	segment.memsz = segment.filesz;
	memcpy(file.data(), &header, sizeof(header));
	memcpy(file.data() + sizeof(header), &segment, sizeof(segment));
	return file;
}
