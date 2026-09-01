#include "x86_assembler.h"

#include <limits>
#include <stdexcept>

using namespace std;

namespace
{

void Byte(vector<unsigned char>& out, unsigned value)
{
	out.push_back(static_cast<unsigned char>(value & 0xff));
}

void Bytes(vector<unsigned char>& out, uint64_t value, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
	{
		Byte(out, static_cast<unsigned>(value));
		value >>= 8;
	}
}

bool FitsSigned8(int64_t value)
{
	return value >= -128 && value <= 127;
}

bool FitsSigned32(int64_t value)
{
	return value >= numeric_limits<int32_t>::min() &&
		value <= numeric_limits<int32_t>::max();
}

unsigned Code(X64Register reg)
{
	return static_cast<unsigned>(reg);
}

void REX(vector<unsigned char>& out, unsigned width, unsigned reg,
	unsigned rm, bool force = false)
{
	const bool w = width == 64;
	const bool r = (reg & 8) != 0;
	const bool b = (rm & 8) != 0;
	if (w || r || b || force)
		Byte(out, 0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (b ? 1 : 0));
}

void Prefix(vector<unsigned char>& out, unsigned width)
{
	if (width == 16)
		Byte(out, 0x66);
	else if (width != 8 && width != 32 && width != 64)
		throw runtime_error("unsupported x86 operand width");
}

void ModRM(vector<unsigned char>& out, unsigned reg_field,
	const X86Operand& operand)
{
	if (operand.kind == X86_REGISTER_OPERAND)
	{
		Byte(out, 0xc0 | ((reg_field & 7) << 3) | (Code(operand.reg) & 7));
		return;
	}
	if (operand.kind != X86_MEMORY_OPERAND)
		throw runtime_error("x86 ModRM requires a register or memory operand");
	const unsigned base = Code(operand.base);
	const int64_t displacement = operand.displacement;
	unsigned mod;
	if (displacement == 0 && (base & 7) != 5)
		mod = 0;
	else if (FitsSigned8(displacement))
		mod = 1;
	else if (FitsSigned32(displacement))
		mod = 2;
	else
		throw runtime_error("x86 memory displacement is too large");
	Byte(out, (mod << 6) | ((reg_field & 7) << 3) |
		((base & 7) == 4 ? 4 : (base & 7)));
	if ((base & 7) == 4)
		Byte(out, 0x24);
	if (mod == 1)
		Byte(out, static_cast<unsigned>(displacement));
	else if (mod == 2 || (mod == 0 && (base & 7) == 5))
		Bytes(out, static_cast<uint32_t>(displacement), 4);
}

bool IsByteRegister(unsigned code)
{
	return code >= 4;
}

unsigned HighByteCode(X64Register reg)
{
	const unsigned code = Code(reg);
	if (code > 3)
		throw runtime_error("x86 high-byte register must be legacy AX, CX, DX, or BX");
	return code + 4;
}

bool IsLegacyByteRm(const X86Operand& operand)
{
	if (operand.kind == X86_REGISTER_OPERAND)
		return Code(operand.reg) <= 3;
	if (operand.kind == X86_MEMORY_OPERAND)
		return Code(operand.base) <= 7;
	return false;
}

void EncodeHighByteMove(vector<unsigned char>& out, unsigned width,
	const vector<X86Operand>& operands)
{
	if (width != 8 || operands.size() != 2)
		throw runtime_error("high-byte moves require two byte operands");
	const X86Operand& dst = operands[0];
	const X86Operand& src = operands[1];
	if (dst.kind == X86_HIGH_BYTE_REGISTER_OPERAND)
	{
		const unsigned high = HighByteCode(dst.reg);
		if (src.kind == X86_IMMEDIATE_OPERAND)
		{
			Prefix(out, width);
			Byte(out, 0xb0 + high);
			Byte(out, static_cast<unsigned>(src.immediate));
			return;
		}
		if (!IsLegacyByteRm(src))
			throw runtime_error("high-byte move source requires a legacy byte operand");
		Prefix(out, width);
		Byte(out, 0x8a);
		ModRM(out, high, src);
		return;
	}
	if (src.kind != X86_HIGH_BYTE_REGISTER_OPERAND ||
		!IsLegacyByteRm(dst))
		throw runtime_error("high-byte move destination requires a legacy byte operand");
	Prefix(out, width);
	Byte(out, 0x88);
	ModRM(out, HighByteCode(src.reg), dst);
}

void EncodeMov(vector<unsigned char>& out, unsigned width,
	const vector<X86Operand>& operands)
{
	if (operands.size() != 2)
		throw runtime_error("MOV requires two operands");
	const X86Operand& dst = operands[0];
	const X86Operand& src = operands[1];
	if (dst.kind == X86_HIGH_BYTE_REGISTER_OPERAND ||
		src.kind == X86_HIGH_BYTE_REGISTER_OPERAND)
	{
		EncodeHighByteMove(out, width, operands);
		return;
	}
	if (dst.kind == X86_REGISTER_OPERAND && src.kind == X86_IMMEDIATE_OPERAND)
	{
		if (width == 64 && !src.force_full_width)
		{
			const unsigned reg = Code(dst.reg);
			if (src.immediate <= numeric_limits<uint32_t>::max())
			{
				REX(out, 32, 0, reg);
				Byte(out, 0xb8 + (reg & 7));
				Bytes(out, src.immediate, 4);
				return;
			}
			if (src.immediate >= uint64_t(0xffffffff80000000ULL))
			{
				REX(out, width, 0, reg);
				Byte(out, 0xc7);
				ModRM(out, 0, dst);
				Bytes(out, src.immediate, 4);
				return;
			}
		}
		Prefix(out, width);
		const unsigned reg = Code(dst.reg);
		const bool force = width == 8 && IsByteRegister(reg);
		REX(out, width, 0, reg, force);
		if (width == 8)
			Byte(out, 0xb0 + (reg & 7));
		else
			Byte(out, 0xb8 + (reg & 7));
		Bytes(out, src.immediate, width / 8);
		return;
	}
	if (src.kind == X86_IMMEDIATE_OPERAND &&
		(dst.kind == X86_REGISTER_OPERAND || dst.kind == X86_MEMORY_OPERAND))
	{
		if (dst.kind == X86_REGISTER_OPERAND && width == 64)
		{
			Prefix(out, width);
			const unsigned reg = Code(dst.reg);
			REX(out, width, 0, reg);
			Byte(out, 0xb8 + (reg & 7));
			Bytes(out, src.immediate, 8);
			return;
		}
		Prefix(out, width);
		const unsigned rm_code = dst.kind == X86_REGISTER_OPERAND ?
			Code(dst.reg) : Code(dst.base);
		const bool force = width == 8 && IsByteRegister(rm_code);
		REX(out, width, 0, rm_code, force);
		Byte(out, width == 8 ? 0xc6 : 0xc7);
		ModRM(out, 0, dst);
		Bytes(out, src.immediate, width / 8 == 8 ? 4 : width / 8);
		return;
	}
	if ((dst.kind != X86_REGISTER_OPERAND && dst.kind != X86_MEMORY_OPERAND) ||
		(src.kind != X86_REGISTER_OPERAND && src.kind != X86_MEMORY_OPERAND))
		throw runtime_error("invalid MOV operands");
	if (dst.kind == X86_MEMORY_OPERAND && src.kind != X86_REGISTER_OPERAND)
		throw runtime_error("memory-to-memory MOV is not encodable");
	if (dst.kind == X86_REGISTER_OPERAND)
	{
		Prefix(out, width);
		const unsigned reg = Code(dst.reg);
		const unsigned rm = src.kind == X86_REGISTER_OPERAND ? Code(src.reg) :
			Code(src.base);
		const bool force = width == 8 && (IsByteRegister(reg) || IsByteRegister(rm));
		REX(out, width, reg, rm, force);
		Byte(out, width == 8 ? 0x8a : 0x8b);
		ModRM(out, reg, src);
	}
	else
	{
		Prefix(out, width);
		const unsigned reg = Code(src.reg);
		const unsigned rm = Code(dst.base);
		const bool force = width == 8 && (IsByteRegister(reg) || IsByteRegister(rm));
		REX(out, width, reg, rm, force);
		Byte(out, width == 8 ? 0x88 : 0x89);
		ModRM(out, reg, dst);
	}
}

unsigned BinaryOpcode(X86Mnemonic mnemonic, unsigned width, bool reg_to_rm)
{
	const unsigned low = width == 8 ? 0 : 1;
	switch (mnemonic)
	{
	case X86_ADD: return reg_to_rm ? 0x00 + low : 0x02 + low;
	case X86_SUB: return reg_to_rm ? 0x28 + low : 0x2a + low;
	case X86_AND: return reg_to_rm ? 0x20 + low : 0x22 + low;
	case X86_OR:  return reg_to_rm ? 0x08 + low : 0x0a + low;
	case X86_XOR: return reg_to_rm ? 0x30 + low : 0x32 + low;
	case X86_CMP: return reg_to_rm ? 0x38 + low : 0x3a + low;
	default: throw runtime_error("not an x86 binary mnemonic");
	}
}

unsigned BinaryImmediateGroup(X86Mnemonic mnemonic)
{
	switch (mnemonic)
	{
	case X86_ADD: return 0;
	case X86_OR:  return 1;
	case X86_AND: return 4;
	case X86_SUB: return 5;
	case X86_XOR: return 6;
	case X86_CMP: return 7;
	default: throw runtime_error("not an x86 immediate binary mnemonic");
	}
}

uint64_t WidthMask(unsigned width)
{
	if (width >= 64)
		return numeric_limits<uint64_t>::max();
	return (uint64_t(1) << width) - 1;
}

bool FitsSignExtendedImmediate(uint64_t value, unsigned width,
	unsigned immediate_width)
{
	const uint64_t mask = WidthMask(width);
	int64_t signed_value;
	if (immediate_width == 8)
		signed_value = static_cast<int8_t>(value);
	else
		signed_value = static_cast<int32_t>(value);
	return (value & mask) ==
		(static_cast<uint64_t>(signed_value) & mask);
}

void EncodeBinaryImmediate(vector<unsigned char>& out, X86Mnemonic mnemonic,
	unsigned width, const vector<X86Operand>& operands)
{
	if (operands.size() != 2 || operands[1].kind != X86_IMMEDIATE_OPERAND ||
		(operands[0].kind != X86_REGISTER_OPERAND &&
			operands[0].kind != X86_MEMORY_OPERAND))
		throw runtime_error("invalid x86 immediate binary operands");
	const unsigned group = BinaryImmediateGroup(mnemonic);
	unsigned opcode;
	unsigned immediate_bytes;
	if (width == 8)
	{
		opcode = 0x80;
		immediate_bytes = 1;
	}
	else if (FitsSignExtendedImmediate(operands[1].immediate, width, 8))
	{
		opcode = 0x83;
		immediate_bytes = 1;
	}
	else
	{
		opcode = 0x81;
		immediate_bytes = width == 16 ? 2 : 4;
		if (width == 64 &&
			!FitsSignExtendedImmediate(operands[1].immediate, width, 32))
			throw runtime_error("x86 64-bit binary immediate is too large");
	}
	Prefix(out, width);
	const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
		Code(operands[0].reg) : Code(operands[0].base);
	REX(out, width, 0, rm, width == 8 && IsByteRegister(rm));
	Byte(out, opcode);
	ModRM(out, group, operands[0]);
	Bytes(out, operands[1].immediate, immediate_bytes);
}

void EncodeBinary(vector<unsigned char>& out, X86Mnemonic mnemonic,
	unsigned width, const vector<X86Operand>& operands)
{
	if (operands.size() != 2)
		throw runtime_error("binary x86 instruction requires two operands");
	const X86Operand& dst = operands[0];
	const X86Operand& src = operands[1];
	if (src.kind == X86_IMMEDIATE_OPERAND)
	{
		EncodeBinaryImmediate(out, mnemonic, width, operands);
		return;
	}
	if (src.kind == X86_REGISTER_OPERAND &&
		(dst.kind == X86_REGISTER_OPERAND || dst.kind == X86_MEMORY_OPERAND))
	{
		Prefix(out, width);
		const unsigned rm = dst.kind == X86_REGISTER_OPERAND ? Code(dst.reg) :
			Code(dst.base);
		REX(out, width, Code(src.reg), rm,
			width == 8 && (IsByteRegister(Code(src.reg)) || IsByteRegister(rm)));
		Byte(out, BinaryOpcode(mnemonic, width, true));
		ModRM(out, Code(src.reg), dst);
		return;
	}
	if (dst.kind == X86_REGISTER_OPERAND &&
		(src.kind == X86_REGISTER_OPERAND || src.kind == X86_MEMORY_OPERAND))
	{
		Prefix(out, width);
		const unsigned rm = src.kind == X86_REGISTER_OPERAND ? Code(src.reg) :
			Code(src.base);
		REX(out, width, Code(dst.reg), rm,
			width == 8 && (IsByteRegister(Code(dst.reg)) || IsByteRegister(rm)));
		Byte(out, BinaryOpcode(mnemonic, width, false));
		ModRM(out, Code(dst.reg), src);
		return;
	}
	throw runtime_error("invalid x86 binary operands");
}

void EncodeUnary(vector<unsigned char>& out, X86Mnemonic mnemonic,
	unsigned width, const vector<X86Operand>& operands)
{
	if (operands.size() != 1 ||
		(operands[0].kind != X86_REGISTER_OPERAND &&
		operands[0].kind != X86_MEMORY_OPERAND))
		throw runtime_error("invalid x86 unary operands");
	Prefix(out, width);
	const unsigned group = mnemonic == X86_NOT ? 2 : 3;
	const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
		Code(operands[0].reg) : Code(operands[0].base);
	REX(out, width, 0, rm, width == 8 && IsByteRegister(rm));
	Byte(out, width == 8 ? 0xf6 : 0xf7);
	ModRM(out, group, operands[0]);
}

void EncodeShift(vector<unsigned char>& out, X86Mnemonic mnemonic,
	unsigned width, const vector<X86Operand>& operands)
{
	if (operands.size() != 2 || operands[1].kind != X86_REGISTER_OPERAND ||
		operands[1].reg != XR_RCX)
		throw runtime_error("CY86 shifts require CL");
	if (operands[0].kind != X86_REGISTER_OPERAND &&
		operands[0].kind != X86_MEMORY_OPERAND)
		throw runtime_error("invalid x86 shift destination");
	Prefix(out, width);
	const unsigned group = mnemonic == X86_SHL ? 4 :
		(mnemonic == X86_SHR ? 5 : 7);
	const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
		Code(operands[0].reg) : Code(operands[0].base);
	REX(out, width, 0, rm, width == 8 && IsByteRegister(rm));
	Byte(out, width == 8 ? 0xd2 : 0xd3);
	ModRM(out, group, operands[0]);
}

void EncodeUnaryMultiply(vector<unsigned char>& out, X86Mnemonic mnemonic,
	unsigned width, const vector<X86Operand>& operands)
{
	if (operands.size() != 1 ||
		(operands[0].kind != X86_REGISTER_OPERAND &&
		operands[0].kind != X86_HIGH_BYTE_REGISTER_OPERAND &&
		operands[0].kind != X86_MEMORY_OPERAND))
		throw runtime_error("invalid x86 multiply/divide operands");
	if (operands[0].kind == X86_HIGH_BYTE_REGISTER_OPERAND && width != 8)
		throw runtime_error("x86 high-byte multiply/divide requires byte width");
	Prefix(out, width);
	const unsigned group = mnemonic == X86_MUL ? 4 :
		(mnemonic == X86_IMUL ? 5 :
		(mnemonic == X86_DIV ? 6 : 7));
	const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
		Code(operands[0].reg) :
		(operands[0].kind == X86_HIGH_BYTE_REGISTER_OPERAND ?
			HighByteCode(operands[0].reg) : Code(operands[0].base));
	if (operands[0].kind != X86_HIGH_BYTE_REGISTER_OPERAND)
		REX(out, width, 0, rm, width == 8 && IsByteRegister(rm));
	Byte(out, width == 8 ? 0xf6 : 0xf7);
	if (operands[0].kind == X86_HIGH_BYTE_REGISTER_OPERAND)
	{
		X86Operand encoded = operands[0];
		encoded.kind = X86_REGISTER_OPERAND;
		encoded.reg = static_cast<X64Register>(rm);
		ModRM(out, group, encoded);
	}
	else
		ModRM(out, group, operands[0]);
}

void EncodeNoOperand(vector<unsigned char>& out, X86Mnemonic mnemonic)
{
	switch (mnemonic)
	{
	case X86_CBW: Byte(out, 0x66); Byte(out, 0x98); return;
	case X86_CWDE: Byte(out, 0x98); return;
	case X86_CDQE: Byte(out, 0x48); Byte(out, 0x98); return;
	case X86_CWD: Byte(out, 0x66); Byte(out, 0x99); return;
	case X86_CDQ: Byte(out, 0x99); return;
	case X86_CQO: Byte(out, 0x48); Byte(out, 0x99); return;
	case X86_RET: Byte(out, 0xc3); return;
	case X86_SYSCALL: Byte(out, 0x0f); Byte(out, 0x05); return;
	case X86_UD2: Byte(out, 0x0f); Byte(out, 0x0b); return;
	default: throw runtime_error("unexpected x86 no-operand instruction");
	}
}

} // namespace

X86Operand X86Reg(X64Register reg, unsigned width)
{
	X86Operand operand;
	operand.kind = X86_REGISTER_OPERAND;
	operand.reg = reg;
	operand.width = width;
	return operand;
}

X86Operand X86HighByte(X64Register reg)
{
	X86Operand operand;
	operand.kind = X86_HIGH_BYTE_REGISTER_OPERAND;
	operand.reg = reg;
	operand.width = 8;
	return operand;
}

X86Operand X86Mem(X64Register base, int64_t displacement, unsigned width)
{
	X86Operand operand;
	operand.kind = X86_MEMORY_OPERAND;
	operand.base = base;
	operand.displacement = displacement;
	operand.width = width;
	return operand;
}

X86Operand X86Imm(uint64_t value, unsigned width)
{
	X86Operand operand;
	operand.kind = X86_IMMEDIATE_OPERAND;
	operand.immediate = value;
	operand.width = width;
	return operand;
}

X86Operand X86ImmFullWidth(uint64_t value, unsigned width)
{
	X86Operand operand = X86Imm(value, width);
	operand.force_full_width = true;
	return operand;
}

X86Operand X86Rel(int64_t displacement)
{
	X86Operand operand;
	operand.kind = X86_RELATIVE_OPERAND;
	operand.immediate = static_cast<uint64_t>(displacement);
	operand.width = 8;
	return operand;
}

X86Instruction::X86Instruction(X86Mnemonic instruction_mnemonic,
	unsigned instruction_width, const vector<X86Operand>& instruction_operands)
	: mnemonic(instruction_mnemonic), width(instruction_width), condition(XC_E),
		operands(instruction_operands)
{
}

X86Instruction::X86Instruction(X86Mnemonic instruction_mnemonic,
	unsigned instruction_width, X86Condition instruction_condition,
	const vector<X86Operand>& instruction_operands)
	: mnemonic(instruction_mnemonic), width(instruction_width),
		condition(instruction_condition), operands(instruction_operands)
{
}

vector<unsigned char> X86Instruction::Encode() const
{
	vector<unsigned char> result;
	switch (mnemonic)
	{
	case X86_MOV:
		EncodeMov(result, width, operands);
		break;
	case X86_ADD:
	case X86_SUB:
	case X86_AND:
	case X86_OR:
	case X86_XOR:
	case X86_CMP:
		EncodeBinary(result, mnemonic, width, operands);
		break;
	case X86_TEST:
		if (operands.size() != 2 || operands[1].kind != X86_REGISTER_OPERAND)
			throw runtime_error("TEST requires r/m and register");
		Prefix(result, width);
		{
			const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
				Code(operands[0].reg) : Code(operands[0].base);
			REX(result, width, Code(operands[1].reg), rm,
				width == 8 && (IsByteRegister(Code(operands[1].reg)) ||
				IsByteRegister(rm)));
		}
		Byte(result, width == 8 ? 0x84 : 0x85);
		ModRM(result, Code(operands[1].reg), operands[0]);
		break;
	case X86_NOT:
	case X86_NEG:
		EncodeUnary(result, mnemonic, width, operands);
		break;
	case X86_SHL:
	case X86_SHR:
	case X86_SAR:
		EncodeShift(result, mnemonic, width, operands);
		break;
	case X86_MUL:
	case X86_IMUL:
	case X86_DIV:
	case X86_IDIV:
		EncodeUnaryMultiply(result, mnemonic, width, operands);
		break;
	case X86_CBW:
	case X86_CWDE:
	case X86_CDQE:
	case X86_CWD:
	case X86_CDQ:
	case X86_CQO:
	case X86_RET:
	case X86_SYSCALL:
	case X86_UD2:
		if (!operands.empty())
			throw runtime_error("x86 instruction takes no operands");
		EncodeNoOperand(result, mnemonic);
		break;
	case X86_SETCC:
		if (operands.size() != 1 ||
			(operands[0].kind != X86_REGISTER_OPERAND &&
			operands[0].kind != X86_MEMORY_OPERAND))
			throw runtime_error("SETcc requires a byte destination");
		{
			const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
				Code(operands[0].reg) : Code(operands[0].base);
			REX(result, 8, 0, rm, IsByteRegister(rm));
		}
		Byte(result, 0x0f);
		Byte(result, 0x90 + static_cast<unsigned>(condition));
		ModRM(result, 0, operands[0]);
		break;
	case X86_JCC_REL8:
		if (operands.size() != 1 || operands[0].kind != X86_RELATIVE_OPERAND ||
			!FitsSigned8(static_cast<int64_t>(operands[0].immediate)))
			throw runtime_error("short conditional jump requires rel8");
		Byte(result, 0x70 + static_cast<unsigned>(condition));
		Byte(result, static_cast<unsigned>(operands[0].immediate));
		break;
	case X86_JMP:
	case X86_CALL:
		if (operands.size() != 1 ||
			(operands[0].kind != X86_REGISTER_OPERAND &&
			operands[0].kind != X86_MEMORY_OPERAND))
			throw runtime_error("indirect jump/call requires r/m operand");
		{
			const unsigned rm = operands[0].kind == X86_REGISTER_OPERAND ?
				Code(operands[0].reg) : Code(operands[0].base);
			REX(result, 0, 0, rm);
			Byte(result, 0xff);
			ModRM(result, mnemonic == X86_JMP ? 4 : 2, operands[0]);
		}
		break;
	case X86_PUSH:
	case X86_POP:
		if (operands.size() != 1 || operands[0].kind != X86_REGISTER_OPERAND)
			throw runtime_error("PUSH/POP requires a register");
		{
			const unsigned reg = Code(operands[0].reg);
			REX(result, 0, 0, reg);
			Byte(result, (mnemonic == X86_PUSH ? 0x50 : 0x58) + (reg & 7));
		}
		break;
	}
	return result;
}
