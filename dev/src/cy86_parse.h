#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"
#include "posttoken_stream.h"
#include "x86_register_model.h"

struct Cy86Error : std::runtime_error
{
	explicit Cy86Error(const std::string& message)
		: std::runtime_error(message)
	{
	}
};

struct Cy86Literal
{
	EFundamentalType type;
	std::vector<unsigned char> bytes;
	std::size_t num_elements;
	bool is_array;

	Cy86Literal()
		: type(FT_INT), num_elements(1), is_array(false)
	{
	}
};

enum Cy86TokenKind
{
	CY86_SIMPLE_TOKEN,
	CY86_IDENTIFIER_TOKEN,
	CY86_LITERAL_TOKEN,
	CY86_EOF_TOKEN
};

struct Cy86Token
{
	Cy86TokenKind kind;
	ETokenType simple_type;
	std::string spelling;
	Cy86Literal literal;

	Cy86Token(Cy86TokenKind token_kind, const std::string& token_spelling,
		ETokenType token_type = KW_AUTO)
		: kind(token_kind), simple_type(token_type), spelling(token_spelling)
	{
	}

	bool IsSimple(ETokenType type) const
	{
		return kind == CY86_SIMPLE_TOKEN && simple_type == type;
	}

	bool IsIdentifier() const
	{
		return kind == CY86_IDENTIFIER_TOKEN;
	}

	bool IsLiteral() const
	{
		return kind == CY86_LITERAL_TOKEN;
	}
};

class Cy86TokenCollector : public IPostTokenOutputStream
{
public:
	std::vector<Cy86Token> tokens;

	void emit_invalid(const std::string& source) override;
	void emit_simple(const std::string& source, ETokenType token_type) override;
	void emit_identifier(const std::string& source) override;
	void emit_literal(const std::string& source, EFundamentalType type,
		const void* data, std::size_t nbytes) override;
	void emit_literal_array(const std::string& source,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) override;
	void emit_user_defined_literal_character(const std::string& source,
		const std::string& ud_suffix, EFundamentalType type, const void* data,
		std::size_t nbytes) override;
	void emit_user_defined_literal_string_array(
		const std::string& source, const std::string& ud_suffix,
		std::size_t num_elements, EFundamentalType type, const void* data,
		std::size_t nbytes) override;
	void emit_user_defined_literal_integer(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) override;
	void emit_user_defined_literal_floating(const std::string& source,
		const std::string& ud_suffix, const std::string& prefix) override;
	void emit_eof() override;

private:
	void AppendLiteral(const std::string& source, EFundamentalType type,
		const void* data, std::size_t nbytes, std::size_t num_elements);
};

struct Cy86OperandSpec
{
	char category;
	char access;
	unsigned width;
	std::string spelling;
};

struct Cy86Opcode
{
	std::string name;
	std::vector<Cy86OperandSpec> operands;
};

const std::vector<Cy86Opcode>& Cy86OpcodeTable();
const Cy86Opcode* Cy86FindOpcode(const std::string& name);

struct Cy86Register
{
	X64Register reg;
	unsigned width;
	std::string spelling;

	Cy86Register()
		: reg(XR_RAX), width(0)
	{
	}
};

bool Cy86ParseRegister(const std::string& spelling, Cy86Register& result);
bool Cy86IsReservedLabel(const std::string& spelling);

struct Cy86Immediate
{
	Cy86Literal literal;
	bool negated;
	bool label;
	std::string label_name;
	bool has_addend;
	bool addend_negative;
	Cy86Literal addend;

	Cy86Immediate()
		: negated(false), label(false), has_addend(false),
			addend_negative(false)
	{
	}
};

struct Cy86Memory
{
	bool base_is_register;
	Cy86Register base_register;
	bool base_is_label;
	std::string base_label;
	bool base_is_literal;
	Cy86Literal base_literal;
	bool has_offset;
	bool offset_negative;
	Cy86Literal offset;

	Cy86Memory()
		: base_is_register(false), base_is_label(false),
			base_is_literal(false), has_offset(false), offset_negative(false)
	{
	}
};

enum Cy86OperandKind
{
	CY86_REGISTER_OPERAND,
	CY86_IMMEDIATE_OPERAND,
	CY86_MEMORY_OPERAND
};

struct Cy86Operand
{
	Cy86OperandKind kind;
	Cy86Register reg;
	Cy86Immediate immediate;
	Cy86Memory memory;
};

struct Cy86Statement
{
	bool is_data;
	std::vector<std::string> labels;
	std::string opcode;
	std::vector<Cy86Operand> operands;
	Cy86Literal literal;
	std::size_t data_width;
	bool negated;

	Cy86Statement()
		: is_data(false), data_width(0), negated(false)
	{
	}
};

class Cy86Parser
{
public:
	explicit Cy86Parser(const std::vector<Cy86Token>& tokens);

	std::vector<Cy86Statement> Parse();

private:
	const Cy86Token& Current() const;
	bool AtEnd() const;
	bool ConsumeSimple(ETokenType type);
	void ExpectSimple(ETokenType type);
	std::string ConsumeIdentifier();
	Cy86Literal ConsumeLiteral();

	Cy86Statement ParseStatement();
	Cy86Statement ParseDataLiteral(const std::vector<std::string>& labels,
		bool negated);
	Cy86Statement ParseInstruction(const std::vector<std::string>& labels,
		const std::string& opcode_name);
	Cy86Operand ParseOperand();
	Cy86Immediate ParseImmediate(bool parenthesized);
	Cy86Memory ParseMemory();
	void ValidateStatement(Cy86Statement& statement);
	void ValidateOperand(const Cy86Operand& operand,
		const Cy86OperandSpec& spec) const;
	void ValidateLabels() const;
	void RememberLabelReference(const Cy86Operand& operand);

	const std::vector<Cy86Token>& tokens_;
	std::size_t position_;
	std::vector<Cy86Statement> statements_;
	std::map<std::string, std::size_t> label_definitions_;
	std::vector<std::string> label_references_;
};

bool Cy86IsSignedIntegral(EFundamentalType type);
bool Cy86IsIntegral(EFundamentalType type);
bool Cy86IsArithmetic(EFundamentalType type);
std::size_t Cy86FundamentalSize(EFundamentalType type);
