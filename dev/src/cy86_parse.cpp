#include "cy86_parse.h"

#include <cctype>
#include <cstring>
#include <sstream>

using namespace std;

namespace
{

const char kOpcodeDescription[] = R"CY86(data8 rI8
data16 rI16
data32 rI32
data64 rI64
move8 w8 r8
move16 w16 r16
move32 w32 r32
move64 w64 r64
move80 w80 r80
jump ar64
jumpif br8 ar64
call ar64
ret
not8 w8 r8
not16 w16 r16
not32 w32 r32
not64 w64 r64
and8 w8 r8 r8
and16 w16 r16 r16
and32 w32 r32 r32
and64 w64 r64 r64
or8 w8 r8 r8
or16 w16 r16 r16
or32 w32 r32 r32
or64 w64 r64 r64
xor8 w8 r8 r8
xor16 w16 r16 r16
xor32 w32 r32 r32
xor64 w64 r64 r64
lshift8 iw8 ir8 ur8
lshift16 iw16 ir16 ur8
lshift32 iw32 ir32 ur8
lshift64 iw64 ir64 ur8
srshift8 sw8 sr8 ur8
srshift16 sw16 sr16 ur8
srshift32 sw32 sr32 ur8
srshift64 sw64 sr64 ur8
urshift8 uw8 ur8 ur8
urshift16 uw16 ur16 ur8
urshift32 uw32 ur32 ur8
urshift64 uw64 ur64 ur8
s8convf80 fw80 sr8
s16convf80 fw80 sr16
s32convf80 fw80 sr32
s64convf80 fw80 sr64
u8convf80 fw80 ur8
u16convf80 fw80 ur16
u32convf80 fw80 ur32
u64convf80 fw80 ur64
f32convf80 fw80 fr32
f64convf80 fw80 fr64
f80convs8 sw8  fr80
f80convs16 sw16 fr80
f80convs32 sw32 fr80
f80convs64 sw64 fr80
f80convu8 uw8 fr80
f80convu16 uw16 fr80
f80convu32 uw32 fr80
f80convu64 uw64 fr80
f80convf32 fw32 fr80
f80convf64 fw64 fr80
iadd8 iw8 ir8 ir8
iadd16 iw16 ir16 ir16
iadd32 iw32 ir32 ir32
iadd64 iw64 ir64 ir64
fadd32 fw32 fr32 fr32
fadd64 fw64 fr64 fr64
fadd80 fw80 fr80 fr80
isub8 iw8 ir8 ir8
isub16 iw16 ir16 ir16
isub32 iw32 ir32 ir32
isub64 iw64 ir64 ir64
fsub32 fw32 fr32 fr32
fsub64 fw64 fr64 fr64
fsub80 fw80 fr80 fr80
smul8 sw8 sr8 sr8
smul16 sw16 sr16 sr16
smul32 sw32 sr32 sr32
smul64 sw64 sr64 sr64
umul8 uw8 ur8 ur8
umul16 uw16 ur16 ur16
umul32 uw32 ur32 ur32
umul64 uw64 ur64 ur64
fmul32 fw32 fr32 fr32
fmul64 fw64 fr64 fr64
fmul80 fw80 fr80 fr80
sdiv8 sw8 sr8 sr8
sdiv16 sw16 sr16 sr16
sdiv32 sw32 sr32 sr32
sdiv64 sw64 sr64 sr64
udiv8 uw8 ur8 ur8
udiv16 uw16 ur16 ur16
udiv32 uw32 ur32 ur32
udiv64 uw64 ur64 ur64
fdiv32 fw32 fr32 fr32
fdiv64 fw64 fr64 fr64
fdiv80 fw80 fr80 fr80
smod8 sw8 sr8 sr8
smod16 sw16 sr16 sr16
smod32 sw32 sr32 sr32
smod64 sw64 sr64 sr64
umod8 uw8 ur8 ur8
umod16 uw16 ur16 ur16
umod32 uw32 ur32 ur32
umod64 uw64 ur64 ur64
ieq8 wb8 ir8 ir8
ieq16 wb8 ir16 ir16
ieq32 wb8 ir32 ir32
ieq64 wb8 ir64 ir64
feq32 wb8 fr32 fr32
feq64 wb8 fr64 fr64
feq80 wb8 fr80 fr80
ine8 wb8 ir8 ir8
ine16 wb8 ir16 ir16
ine32 wb8 ir32 ir32
ine64 wb8 ir64 ir64
fne32 wb8 fr32 fr32
fne64 wb8 fr64 fr64
fne80 wb8 fr80 fr80
slt8 wb8 sr8 sr8
slt16 wb8 sr16 sr16
slt32 wb8 sr32 sr32
slt64 wb8 sr64 sr64
ult8 wb8 ur8 ur8
ult16 wb8 ur16 ur16
ult32 wb8 ur32 ur32
ult64 wb8 ur64 ur64
flt32 wb8 fr32 fr32
flt64 wb8 fr64 fr64
flt80 wb8 fr80 fr80
sgt8 wb8 sr8 sr8
sgt16 wb8 sr16 sr16
sgt32 wb8 sr32 sr32
sgt64 wb8 sr64 sr64
ugt8 wb8 ur8 ur8
ugt16 wb8 ur16 ur16
ugt32 wb8 ur32 ur32
ugt64 wb8 ur64 ur64
fgt32 wb8 fr32 fr32
fgt64 wb8 fr64 fr64
fgt80 wb8 fr80 fr80
sle8 wb8 sr8 sr8
sle16 wb8 sr16 sr16
sle32 wb8 sr32 sr32
sle64 wb8 sr64 sr64
ule8 wb8 ur8 ur8
ule16 wb8 ur16 ur16
ule32 wb8 ur32 ur32
ule64 wb8 ur64 ur64
fle32 wb8 fr32 fr32
fle64 wb8 fr64 fr64
fle80 wb8 fr80 fr80
sge8 wb8 sr8 sr8
sge16 wb8 sr16 sr16
sge32 wb8 sr32 sr32
sge64 wb8 sr64 sr64
uge8 wb8 ur8 ur8
uge16 wb8 ur16 ur16
uge32 wb8 ur32 ur32
uge64 wb8 ur64 ur64
fge32 wb8 fr32 fr32
fge64 wb8 fr64 fr64
fge80 wb8 fr80 fr80
syscall0 w64 r64
syscall1 w64 r64 r64
syscall2 w64 r64 r64 r64
syscall3 w64 r64 r64 r64 r64
syscall4 w64 r64 r64 r64 r64 r64
syscall5 w64 r64 r64 r64 r64 r64 r64
syscall6 w64 r64 r64 r64 r64 r64 r64 r64
)CY86";

bool IsKeyword(ETokenType type)
{
	return type >= KW_ALIGNAS && type <= KW_WHILE;
}

unsigned ParseWidth(const string& spelling, string& prefix)
{
	size_t first_digit = spelling.find_first_of("0123456789");
	if (first_digit == string::npos || first_digit == 0)
		throw Cy86Error("invalid opcode operand descriptor: " + spelling);
	prefix = spelling.substr(0, first_digit);
	unsigned width = 0;
	for (size_t i = first_digit; i < spelling.size(); ++i)
	{
		if (!isdigit(static_cast<unsigned char>(spelling[i])))
			throw Cy86Error("invalid opcode operand descriptor: " + spelling);
		width = width * 10 + static_cast<unsigned>(spelling[i] - '0');
	}
	if (width != 8 && width != 16 && width != 32 && width != 64 &&
		width != 80)
		throw Cy86Error("invalid opcode operand width: " + spelling);
	return width;
}

vector<Cy86Opcode> ParseOpcodeTable()
{
	vector<Cy86Opcode> result;
	istringstream input(kOpcodeDescription);
	string line;
	while (getline(input, line))
	{
		istringstream row(line);
		string name;
		if (!(row >> name))
			continue;
		Cy86Opcode opcode;
		opcode.name = name;
		string descriptor;
		while (row >> descriptor)
		{
			string prefix;
			Cy86OperandSpec spec;
			spec.width = ParseWidth(descriptor, prefix);
			if (prefix.size() == 1)
			{
				spec.category = 0;
				spec.access = prefix[0];
			}
			else if (prefix.size() == 2)
			{
				spec.category = prefix[0];
				spec.access = prefix[1];
			}
			else
				throw Cy86Error("invalid opcode operand descriptor: " + descriptor);
			spec.spelling = descriptor;
			opcode.operands.push_back(spec);
		}
		result.push_back(opcode);
	}
	if (result.size() != 170)
		throw Cy86Error("CY86 opcode table is incomplete");
	return result;
}

void RejectIfNotArithmetic(const Cy86Literal& literal)
{
	if (!Cy86IsArithmetic(literal.type) || Cy86IsLiteralArray(literal))
		throw Cy86Error("literal is not arithmetic");
}

} // namespace

void Cy86TokenCollector::emit_invalid(const string& source)
{
	throw Cy86Error("invalid posttoken: " + source);
}

void Cy86TokenCollector::emit_simple(const string& source,
	ETokenType token_type)
{
	if (IsKeyword(token_type))
		throw Cy86Error("keyword is not valid CY86 syntax: " + source);
	tokens.push_back(Cy86Token(CY86_SIMPLE_TOKEN, source, token_type));
}

void Cy86TokenCollector::emit_identifier(const string& source)
{
	tokens.push_back(Cy86Token(CY86_IDENTIFIER_TOKEN, source));
}

void Cy86TokenCollector::AppendLiteral(const string& source,
	EFundamentalType type, const void* data, size_t nbytes,
	size_t num_elements)
{
	Cy86Token token(CY86_LITERAL_TOKEN, source);
	token.literal.type = type;
	token.literal.num_elements = num_elements;
	token.literal.is_array = false;
	token.literal.bytes.resize(nbytes);
	if (data != 0 && nbytes != 0)
		memcpy(token.literal.bytes.data(), data, nbytes);
	tokens.push_back(token);
}

void Cy86TokenCollector::emit_literal(const string& source,
	EFundamentalType type, const void* data, size_t nbytes)
{
	AppendLiteral(source, type, data, nbytes, 1);
}

void Cy86TokenCollector::emit_literal_array(const string& source,
	size_t num_elements, EFundamentalType type, const void* data,
	size_t nbytes)
{
	AppendLiteral(source, type, data, nbytes, num_elements);
	tokens.back().literal.is_array = true;
}

void Cy86TokenCollector::emit_user_defined_literal_character(
	const string&, const string&, EFundamentalType, const void*, size_t)
{
	throw Cy86Error("user-defined literals are not valid CY86 syntax");
}

void Cy86TokenCollector::emit_user_defined_literal_string_array(
	const string&, const string&, size_t, EFundamentalType, const void*, size_t)
{
	throw Cy86Error("user-defined literals are not valid CY86 syntax");
}

void Cy86TokenCollector::emit_user_defined_literal_integer(
	const string&, const string&, const string&)
{
	throw Cy86Error("user-defined literals are not valid CY86 syntax");
}

void Cy86TokenCollector::emit_user_defined_literal_floating(
	const string&, const string&, const string&)
{
	throw Cy86Error("user-defined literals are not valid CY86 syntax");
}

void Cy86TokenCollector::emit_eof()
{
	tokens.push_back(Cy86Token(CY86_EOF_TOKEN, ""));
}

const vector<Cy86Opcode>& Cy86OpcodeTable()
{
	static const vector<Cy86Opcode> table = ParseOpcodeTable();
	return table;
}

const Cy86Opcode* Cy86FindOpcode(const string& name)
{
	static const map<string, const Cy86Opcode*> index = []
	{
		map<string, const Cy86Opcode*> result;
		const vector<Cy86Opcode>& table = Cy86OpcodeTable();
		for (size_t i = 0; i < table.size(); ++i)
			result[table[i].name] = &table[i];
		return result;
	}();
	const map<string, const Cy86Opcode*>::const_iterator it = index.find(name);
	return it == index.end() ? 0 : it->second;
}

bool Cy86ParseRegister(const string& spelling, Cy86Register& result)
{
	if (spelling == "sp")
	{
		result.reg = XR_RSP;
		result.width = 64;
		result.spelling = spelling;
		return true;
	}
	if (spelling == "bp")
	{
		result.reg = XR_RBP;
		result.width = 64;
		result.spelling = spelling;
		return true;
	}
	if (spelling.size() < 2)
		return false;
	size_t digit = spelling.find_first_of("0123456789");
	if (digit == string::npos || digit == 0)
		return false;
	string base = spelling.substr(0, digit);
	unsigned width = 0;
	for (size_t i = digit; i < spelling.size(); ++i)
	{
		if (!isdigit(static_cast<unsigned char>(spelling[i])))
			return false;
		width = width * 10 + static_cast<unsigned>(spelling[i] - '0');
	}
	if (width != 8 && width != 16 && width != 32 && width != 64)
		return false;
	X64Register reg;
	if (base == "x")
		reg = XR_R12;
	else if (base == "y")
		reg = XR_R13;
	else if (base == "z")
		reg = XR_R14;
	else if (base == "t")
		reg = XR_R15;
	else
		return false;
	result.reg = reg;
	result.width = width;
	result.spelling = spelling;
	return true;
}

bool Cy86IsReservedLabel(const string& spelling)
{
	Cy86Register reg;
	return Cy86FindOpcode(spelling) != 0 || Cy86ParseRegister(spelling, reg);
}

bool Cy86IsSignedIntegral(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_CHAR:
	case FT_WCHAR_T:
		return true;
	default:
		return false;
	}
}

bool Cy86IsIntegral(EFundamentalType type)
{
	return Cy86IsSignedIntegral(type) || type == FT_UNSIGNED_CHAR ||
		type == FT_UNSIGNED_SHORT_INT || type == FT_UNSIGNED_INT ||
		type == FT_UNSIGNED_LONG_INT || type == FT_UNSIGNED_LONG_LONG_INT ||
		type == FT_CHAR16_T || type == FT_CHAR32_T || type == FT_BOOL;
}

bool Cy86IsArithmetic(EFundamentalType type)
{
	return Cy86IsIntegral(type) || type == FT_FLOAT || type == FT_DOUBLE ||
		type == FT_LONG_DOUBLE;
}

bool Cy86IsLiteralArray(const Cy86Literal& literal)
{
	return literal.is_array || literal.num_elements > 1;
}

size_t Cy86FundamentalSize(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
	case FT_CHAR:
	case FT_BOOL:
		return 1;
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T:
		return 2;
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_CHAR32_T:
	case FT_WCHAR_T:
		return 4;
	case FT_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_DOUBLE:
		return 8;
	case FT_FLOAT:
		return 4;
	case FT_LONG_DOUBLE:
		return 10;
	default:
		return 0;
	}
}

Cy86Parser::Cy86Parser(const vector<Cy86Token>& tokens)
	: tokens_(tokens), position_(0)
{
}

const Cy86Token& Cy86Parser::Current() const
{
	if (tokens_.empty())
		throw Cy86Error("empty CY86 token stream");
	return tokens_[position_ < tokens_.size() ? position_ : tokens_.size() - 1];
}

bool Cy86Parser::AtEnd() const
{
	return Current().kind == CY86_EOF_TOKEN;
}

bool Cy86Parser::ConsumeSimple(ETokenType type)
{
	if (!Current().IsSimple(type))
		return false;
	++position_;
	return true;
}

void Cy86Parser::ExpectSimple(ETokenType type)
{
	if (!ConsumeSimple(type))
		throw Cy86Error("unexpected token in CY86 statement");
}

string Cy86Parser::ConsumeIdentifier()
{
	if (!Current().IsIdentifier())
		throw Cy86Error("identifier expected in CY86 statement");
	return tokens_[position_++].spelling;
}

Cy86Literal Cy86Parser::ConsumeLiteral()
{
	if (!Current().IsLiteral())
		throw Cy86Error("literal expected in CY86 statement");
	return tokens_[position_++].literal;
}

vector<Cy86Statement> Cy86Parser::Parse()
{
	position_ = 0;
	statements_.clear();
	label_definitions_.clear();
	label_references_.clear();
	if (tokens_.empty())
		throw Cy86Error("missing CY86 end token");
	while (!AtEnd())
	{
		Cy86Statement statement = ParseStatement();
		for (size_t i = 0; i < statement.labels.size(); ++i)
		{
			const string& label = statement.labels[i];
			if (Cy86IsReservedLabel(label) ||
				label_definitions_.find(label) != label_definitions_.end())
				throw Cy86Error("invalid or duplicate CY86 label: " + label);
			label_definitions_[label] = statements_.size();
		}
		statements_.push_back(statement);
	}
	ValidateLabels();
	return statements_;
}

Cy86Statement Cy86Parser::ParseStatement()
{
	vector<string> labels;
	while (Current().IsIdentifier() && position_ + 1 < tokens_.size() &&
		tokens_[position_ + 1].IsSimple(OP_COLON))
	{
		labels.push_back(ConsumeIdentifier());
		ExpectSimple(OP_COLON);
	}
	if (Current().IsLiteral())
	{
		Cy86Statement result = ParseDataLiteral(labels, false);
		ExpectSimple(OP_SEMICOLON);
		return result;
	}
	if (ConsumeSimple(OP_MINUS))
	{
		Cy86Statement result = ParseDataLiteral(labels, true);
		ExpectSimple(OP_SEMICOLON);
		return result;
	}
	if (!Current().IsIdentifier())
		throw Cy86Error("opcode or literal expected in CY86 statement near '" +
			Current().spelling + "'");
	const string opcode_name = ConsumeIdentifier();
	return ParseInstruction(labels, opcode_name);
}

Cy86Statement Cy86Parser::ParseDataLiteral(const vector<string>& labels,
	bool negated)
{
	Cy86Statement result;
	result.is_data = true;
	result.labels = labels;
	result.literal = ConsumeLiteral();
	result.negated = negated;
	if (negated)
		RejectIfNotArithmetic(result.literal);
	return result;
}

Cy86Statement Cy86Parser::ParseInstruction(const vector<string>& labels,
	const string& opcode_name)
{
	Cy86Statement result;
	result.labels = labels;
	result.opcode = opcode_name;
	while (!Current().IsSimple(OP_SEMICOLON))
	{
		if (AtEnd())
			throw Cy86Error("unterminated CY86 statement");
		result.operands.push_back(ParseOperand());
	}
	ExpectSimple(OP_SEMICOLON);
	ValidateStatement(result);
	return result;
}

Cy86Operand Cy86Parser::ParseOperand()
{
	Cy86Operand result;
	if (Current().IsSimple(OP_LSQUARE))
	{
		result.kind = CY86_MEMORY_OPERAND;
		result.memory = ParseMemory();
		return result;
	}
	if (Current().IsSimple(OP_LPAREN))
	{
		result.kind = CY86_IMMEDIATE_OPERAND;
		result.immediate = ParseImmediate(true);
		return result;
	}
	if (Current().IsLiteral())
	{
		result.kind = CY86_IMMEDIATE_OPERAND;
		result.immediate = ParseImmediate(false);
		return result;
	}
	if (Current().IsIdentifier())
	{
		const string spelling = Current().spelling;
		Cy86Register reg;
		if (Cy86ParseRegister(spelling, reg))
		{
			++position_;
			result.kind = CY86_REGISTER_OPERAND;
			result.reg = reg;
			return result;
		}
		result.kind = CY86_IMMEDIATE_OPERAND;
		result.immediate = ParseImmediate(false);
		return result;
	}
	throw Cy86Error("invalid CY86 operand");
}

Cy86Immediate Cy86Parser::ParseImmediate(bool parenthesized)
{
	Cy86Immediate result;
	if (!parenthesized)
	{
		if (Current().IsLiteral())
			result.literal = ConsumeLiteral();
		else
		{
			result.label = true;
			result.label_name = ConsumeIdentifier();
		}
		return result;
	}
	ExpectSimple(OP_LPAREN);
	if (Current().IsLiteral())
	{
		result.literal = ConsumeLiteral();
	}
	else if (ConsumeSimple(OP_MINUS))
	{
		result.literal = ConsumeLiteral();
		result.negated = true;
		RejectIfNotArithmetic(result.literal);
	}
	else if (Current().IsIdentifier())
	{
		result.label = true;
		result.label_name = ConsumeIdentifier();
		if (ConsumeSimple(OP_PLUS))
		{
			result.has_addend = true;
			result.addend = ConsumeLiteral();
		}
		else if (ConsumeSimple(OP_MINUS))
		{
			result.has_addend = true;
			result.addend_negative = true;
			result.addend = ConsumeLiteral();
		}
	}
	else
		throw Cy86Error("invalid parenthesized CY86 immediate");
	ExpectSimple(OP_RPAREN);
	return result;
}

Cy86Memory Cy86Parser::ParseMemory()
{
	Cy86Memory result;
	ExpectSimple(OP_LSQUARE);
	if (Current().IsLiteral())
	{
		result.base_is_literal = true;
		result.base_literal = ConsumeLiteral();
	}
	else if (Current().IsIdentifier())
	{
		const string spelling = Current().spelling;
		Cy86Register reg;
		if (Cy86ParseRegister(spelling, reg))
		{
			result.base_is_register = true;
			result.base_register = reg;
			++position_;
		}
		else
		{
			result.base_is_label = true;
			result.base_label = ConsumeIdentifier();
		}
	}
	else
		throw Cy86Error("invalid CY86 memory base");
	if (ConsumeSimple(OP_PLUS))
	{
		if (result.base_is_literal)
			throw Cy86Error("literal memory base cannot have an offset");
		result.has_offset = true;
		result.offset = ConsumeLiteral();
	}
	else if (ConsumeSimple(OP_MINUS))
	{
		if (result.base_is_literal)
			throw Cy86Error("negative literal memory base is invalid");
		result.has_offset = true;
		result.offset_negative = true;
		result.offset = ConsumeLiteral();
	}
	ExpectSimple(OP_RSQUARE);
	return result;
}

void Cy86Parser::ValidateStatement(Cy86Statement& statement)
{
	const Cy86Opcode* opcode = Cy86FindOpcode(statement.opcode);
	if (opcode == 0)
		throw Cy86Error("unknown CY86 opcode: " + statement.opcode);
	if (statement.opcode.compare(0, 4, "data") == 0)
	{
		if (opcode->operands.size() != 1 || statement.operands.size() != 1 ||
			statement.operands[0].kind != CY86_IMMEDIATE_OPERAND)
			throw Cy86Error("data opcode requires one immediate");
		string suffix = statement.opcode.substr(4);
		unsigned bits = 0;
		for (size_t i = 0; i < suffix.size(); ++i)
		{
			if (!isdigit(static_cast<unsigned char>(suffix[i])))
				throw Cy86Error("invalid data width");
			bits = bits * 10 + static_cast<unsigned>(suffix[i] - '0');
		}
		statement.is_data = true;
		statement.data_width = bits / 8;
		RememberLabelReference(statement.operands[0]);
		return;
	}
	if (statement.operands.size() != opcode->operands.size())
		throw Cy86Error("wrong number of CY86 operands");
	for (size_t i = 0; i < statement.operands.size(); ++i)
	{
		ValidateOperand(statement.operands[i], opcode->operands[i]);
		RememberLabelReference(statement.operands[i]);
	}
}

// Operand types beyond these checks are unconstrained: the reference
// converts every immediate byte-mechanically at the operand width (floats,
// arrays, and labels included) in every category, and floating operands may
// be registers (bit-pattern semantics via a bounce slot).
void Cy86Parser::ValidateOperand(const Cy86Operand& operand,
	const Cy86OperandSpec& spec) const
{
	if (spec.access == 'w' && operand.kind == CY86_IMMEDIATE_OPERAND)
		throw Cy86Error("write operand cannot be immediate");
	if (spec.access == 'I' && operand.kind != CY86_IMMEDIATE_OPERAND)
		throw Cy86Error("operand must be an immediate literal");
	if (operand.kind == CY86_REGISTER_OPERAND &&
		(spec.width == 80 || operand.reg.width != spec.width))
		throw Cy86Error("CY86 register width mismatch");
	if (operand.kind == CY86_MEMORY_OPERAND &&
		operand.memory.base_is_register &&
		operand.memory.base_register.width != 64)
		throw Cy86Error("memory address register must be 64-bit");
}

void Cy86Parser::RememberLabelReference(const Cy86Operand& operand)
{
	if (operand.kind == CY86_IMMEDIATE_OPERAND && operand.immediate.label)
		label_references_.push_back(operand.immediate.label_name);
	if (operand.kind == CY86_MEMORY_OPERAND && operand.memory.base_is_label)
		label_references_.push_back(operand.memory.base_label);
}

void Cy86Parser::ValidateLabels() const
{
	for (size_t i = 0; i < label_references_.size(); ++i)
		if (label_definitions_.find(label_references_[i]) ==
			label_definitions_.end())
			throw Cy86Error("undefined CY86 label: " + label_references_[i]);
}
