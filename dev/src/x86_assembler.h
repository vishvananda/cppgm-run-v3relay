#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "x86_register_model.h"

enum X86Mnemonic
{
	X86_MOV,
	X86_ADD,
	X86_SUB,
	X86_AND,
	X86_OR,
	X86_XOR,
	X86_CMP,
	X86_TEST,
	X86_NOT,
	X86_NEG,
	X86_SHL,
	X86_SHR,
	X86_SAR,
	X86_MUL,
	X86_IMUL,
	X86_DIV,
	X86_IDIV,
	X86_CBW,
	X86_CWDE,
	X86_CDQE,
	X86_CWD,
	X86_CDQ,
	X86_CQO,
	X86_SETCC,
	X86_JCC_REL8,
	X86_JMP,
	X86_CALL,
	X86_RET,
	X86_PUSH,
	X86_POP,
	X86_SYSCALL,
	X86_UD2,
	X86_FLD,
	X86_FSTP,
	X86_FILD,
	X86_FISTP,
	X86_FADD,
	X86_FSUB,
	X86_FMUL,
	X86_FDIV,
	X86_FADDP,
	X86_FSUBP,
	X86_FMULP,
	X86_FDIVP,
	X86_FCOMIP,
	X86_FCHS
};
enum X86OperandKind
{
	X86_REGISTER_OPERAND,
	X86_HIGH_BYTE_REGISTER_OPERAND,
	X86_X87_REGISTER_OPERAND,
	X86_MEMORY_OPERAND,
	X86_IMMEDIATE_OPERAND,
	X86_RELATIVE_OPERAND
};

struct X86Operand
{
	X86OperandKind kind;
	X64Register reg;
	X64Register base;
	unsigned x87_index;
	std::int64_t displacement;
	std::uint64_t immediate;
	unsigned width;
	bool force_full_width;

	X86Operand()
		: kind(X86_IMMEDIATE_OPERAND), reg(XR_RAX), base(XR_RAX),
			x87_index(0), displacement(0), immediate(0), width(0),
			force_full_width(false)
	{
	}
};

X86Operand X86Reg(X64Register reg, unsigned width);
X86Operand X86HighByte(X64Register reg);
X86Operand X87Reg(unsigned index);
X86Operand X86Mem(X64Register base, std::int64_t displacement,
	unsigned width);
X86Operand X86Imm(std::uint64_t value, unsigned width);
X86Operand X86ImmFullWidth(std::uint64_t value, unsigned width);
X86Operand X86Rel(std::int64_t displacement);

struct X86Instruction
{
	X86Mnemonic mnemonic;
	unsigned width;
	X86Condition condition;
	std::vector<X86Operand> operands;

	X86Instruction(X86Mnemonic mnemonic, unsigned width,
		const std::vector<X86Operand>& operands);
	X86Instruction(X86Mnemonic mnemonic, unsigned width,
		X86Condition condition, const std::vector<X86Operand>& operands);

	std::vector<unsigned char> Encode() const;
};
