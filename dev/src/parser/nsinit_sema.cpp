#include "nsinit_sema.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace
{

Pa7TypePtr StripTopCV(const Pa7TypePtr& type)
{
	Pa7TypePtr result = type;
	while (result && result->kind == PA7_TYPE_CV &&
		!result->children.empty())
		result = result->children[0];
	return result;
}

bool HasTopConst(const Pa7TypePtr& type)
{
	return type && type->kind == PA7_TYPE_CV &&
		(type->cv & PA7_CV_CONST) != 0;
}

bool IsIntegralFundamental(EFundamentalType type)
{
	return type <= FT_BOOL;
}

bool IsFloatingFundamental(EFundamentalType type)
{
	return type == FT_FLOAT || type == FT_DOUBLE ||
		type == FT_LONG_DOUBLE;
}

bool CompatibleTypes(const Pa7TypePtr& first, const Pa7TypePtr& second)
{
	if (SameType(first, second))
		return true;
	if (!first || !second || first->kind != second->kind)
		return false;
	if (first->kind == PA7_TYPE_ARRAY && first->children.size() == 1 &&
		second->children.size() == 1)
		return (!first->has_bound || !second->has_bound ||
			(first->bound_expression || second->bound_expression ?
				first->bound_expression == second->bound_expression :
				first->bound == second->bound)) &&
			CompatibleTypes(first->children[0], second->children[0]);
	return false;
}

bool IsInternalVariable(const Pa7Variable& variable)
{
	if (variable.linkage == PA7_LINKAGE_INTERNAL)
		return true;
	if (variable.linkage == PA7_LINKAGE_EXTERNAL)
		return false;
	return variable.is_const || HasTopConst(variable.type) ||
		(variable.owner && variable.owner->unnamed);
}

bool IsInternalFunction(const Pa7Function& function)
{
	if (function.linkage == PA7_LINKAGE_INTERNAL)
		return true;
	if (function.linkage == PA7_LINKAGE_EXTERNAL)
		return false;
	return function.owner && function.owner->unnamed;
}

void AppendLittleEndian(vector<unsigned char>& bytes, uint64_t value,
	size_t width)
{
	bytes.resize(width, 0);
	for (size_t i = 0; i < width; ++i)
		bytes[i] = static_cast<unsigned char>(value >> (i * 8));
}

uint64_t ReadLittleEndian(const vector<unsigned char>& bytes)
{
	uint64_t value = 0;
	const size_t width = bytes.size() < sizeof(value) ? bytes.size() :
		sizeof(value);
	for (size_t i = 0; i < width; ++i)
		value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
	return value;
}

int HexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

} // namespace

Pa8Value::Pa8Value()
	: type(), is_constant(false), is_lvalue(false), is_string_literal(false),
		string_symbol()
{
}

Pa8ProgramEntity::Pa8ProgramEntity()
	: is_function(false), key(), variable(0), function(0), type(), variables(),
		functions(), first_unit(numeric_limits<size_t>::max()),
		first_order(numeric_limits<size_t>::max()), defined(false), offset(0),
		value()
{
}

Pa8ProgramSema::Pa8ProgramSema(
	const vector<shared_ptr<Pa7Namespace> >& globals)
	: globals_(globals), entities_(), strings_(), entity_by_key_(),
		variable_entities_(), function_entities_(), variable_states_(),
		string_symbols_(), next_string_id_(0)
{
}

const vector<Pa8ProgramEntity>& Pa8ProgramSema::Entities() const
{
	return entities_;
}

const vector<Pa8StringLiteral>& Pa8ProgramSema::Strings() const
{
	return strings_;
}

string Pa8ProgramSema::NamespacePath(const Pa7Namespace* scope) const
{
	vector<string> parts;
	for (const Pa7Namespace* current = scope; current && current->parent;
		current = current->parent)
		parts.push_back(current->unnamed ? string("<unnamed>") : current->name);
	string result;
	for (vector<string>::const_reverse_iterator it = parts.rbegin();
		it != parts.rend(); ++it)
	{
		if (!result.empty())
			result += "::";
		result += *it;
	}
	return result;
}

string Pa8ProgramSema::TypeKey(const Pa7TypePtr& type) const
{
	if (!type)
		return "?";
	ostringstream out;
	switch (type->kind)
	{
	case PA7_TYPE_FUNDAMENTAL:
		out << "f" << static_cast<int>(type->fundamental);
		break;
	case PA7_TYPE_CV:
		out << "c" << type->cv << "(" << TypeKey(type->children.empty() ?
			Pa7TypePtr() : type->children[0]) << ")";
		break;
	case PA7_TYPE_POINTER:
		out << "p(" << TypeKey(type->children.empty() ? Pa7TypePtr() :
			type->children[0]) << ")";
		break;
	case PA7_TYPE_LVALUE_REFERENCE:
		out << "l(" << TypeKey(type->children.empty() ? Pa7TypePtr() :
			type->children[0]) << ")";
		break;
	case PA7_TYPE_RVALUE_REFERENCE:
		out << "r(" << TypeKey(type->children.empty() ? Pa7TypePtr() :
			type->children[0]) << ")";
		break;
	case PA7_TYPE_ARRAY:
		out << "a" << (type->has_bound ? type->bound : 0) << "(" <<
			TypeKey(type->children.empty() ? Pa7TypePtr() : type->children[0]) <<
			")";
		break;
	case PA7_TYPE_FUNCTION:
		out << "F(";
		for (size_t i = 0; i < type->children.size(); ++i)
			out << TypeKey(type->children[i]) << ";";
		out << ")" << (type->varargs ? "v" : "n") << "->" <<
			TypeKey(type->return_type);
		break;
	}
	return out.str();
}

string Pa8ProgramSema::MakeVariableKey(const Pa7Variable* variable,
	size_t unit) const
{
	if (!variable)
		return string();
	const bool internal = IsInternalVariable(*variable);
	ostringstream out;
	out << "V|" << (internal ? "I|" : "E|");
	if (internal)
		out << unit << "|";
	out << NamespacePath(variable->owner) << "|" << variable->name;
	return out.str();
}

string Pa8ProgramSema::MakeFunctionKey(const Pa7Function* function,
	size_t unit) const
{
	if (!function)
		return string();
	const bool internal = IsInternalFunction(*function);
	ostringstream out;
	out << "F|" << (internal ? "I|" : "E|");
	if (internal)
		out << unit << "|";
	out << NamespacePath(function->owner) << "|" << function->name << "|" <<
		TypeKey(function->type);
	return out.str();
}

Pa8ProgramEntity* Pa8ProgramSema::FindOrCreateEntity(const string& key,
	bool is_function, size_t unit, size_t order)
{
	map<string, size_t>::iterator found = entity_by_key_.find(key);
	if (found != entity_by_key_.end())
	{
		Pa8ProgramEntity& entity = entities_[found->second];
		if (entity.is_function != is_function)
			throw runtime_error("program entity kind conflict");
		if (unit < entity.first_unit ||
			(unit == entity.first_unit && order < entity.first_order))
		{
			entity.first_unit = unit;
			entity.first_order = order;
		}
		return &entity;
	}
	Pa8ProgramEntity entity;
	entity.is_function = is_function;
	entity.key = key;
	entity.first_unit = unit;
	entity.first_order = order;
	entities_.push_back(entity);
	const size_t index = entities_.size() - 1;
	entity_by_key_[key] = index;
	return &entities_[index];
}

void Pa8ProgramSema::AddVariable(Pa7Variable* variable, size_t unit)
{
	const string key = MakeVariableKey(variable, unit);
	Pa8ProgramEntity* entity = FindOrCreateEntity(key, false, unit,
		variable->order);
	entity->variables.push_back(variable);
	if (!entity->variable)
		entity->variable = variable;
	if (!entity->type)
		entity->type = variable->type;
	else if (!CompatibleTypes(entity->type, variable->type))
		throw runtime_error("variable type does not agree across translation units");
	else
		entity->type = MergeTypes(entity->type, variable->type);
	if (variable->defined)
	{
		if (entity->defined)
			throw runtime_error("more than one variable definition");
		entity->defined = true;
		entity->variable = variable;
	}
}

void Pa8ProgramSema::AddFunction(Pa7Function* function, size_t unit)
{
	const string key = MakeFunctionKey(function, unit);
	Pa8ProgramEntity* entity = FindOrCreateEntity(key, true, unit,
		function->order);
	entity->functions.push_back(function);
	if (!entity->function)
		entity->function = function;
	if (function->defined)
	{
		if (entity->defined && !(entity->function && entity->function->is_inline &&
			function->is_inline))
			throw runtime_error("more than one function definition");
		entity->defined = true;
		if (!entity->function->defined)
			entity->function = function;
	}
}

void Pa8ProgramSema::CollectNamespace(Pa7Namespace* scope, size_t unit)
{
	if (!scope)
		return;
	for (size_t i = 0; i < scope->variables.size(); ++i)
		AddVariable(scope->variables[i].get(), unit);
	for (size_t i = 0; i < scope->functions.size(); ++i)
		AddFunction(scope->functions[i].get(), unit);
	for (size_t i = 0; i < scope->namespaces.size(); ++i)
		CollectNamespace(scope->namespaces[i].get(), unit);
}

string Pa8ProgramSema::SymbolFor(const Pa7Variable* variable) const
{
	map<const Pa7Variable*, size_t>::const_iterator it =
		variable_entities_.find(variable);
	return it == variable_entities_.end() ? string() :
		entities_[it->second].key;
}

string Pa8ProgramSema::SymbolFor(const Pa7Function* function) const
{
	map<const Pa7Function*, size_t>::const_iterator it =
		function_entities_.find(function);
	return it == function_entities_.end() ? string() :
		entities_[it->second].key;
}

Pa7TypePtr Pa8ProgramSema::StripCV(const Pa7TypePtr& type) const
{
	return StripTopCV(type);
}

bool Pa8ProgramSema::IsConstType(const Pa7TypePtr& type) const
{
	return HasTopConst(type);
}

bool Pa8ProgramSema::IsIntegralType(const Pa7TypePtr& type) const
{
	Pa7TypePtr stripped = StripCV(type);
	return stripped && stripped->kind == PA7_TYPE_FUNDAMENTAL &&
		IsIntegralFundamental(stripped->fundamental);
}

bool Pa8ProgramSema::IsFloatingType(const Pa7TypePtr& type) const
{
	Pa7TypePtr stripped = StripCV(type);
	return stripped && stripped->kind == PA7_TYPE_FUNDAMENTAL &&
		IsFloatingFundamental(stripped->fundamental);
}

Pa7TypePtr Pa8ProgramSema::Pointee(const Pa7TypePtr& type) const
{
	Pa7TypePtr stripped = StripCV(type);
	if (!stripped || (stripped->kind != PA7_TYPE_POINTER &&
		stripped->kind != PA7_TYPE_LVALUE_REFERENCE &&
		stripped->kind != PA7_TYPE_RVALUE_REFERENCE) ||
		stripped->children.size() != 1)
		return Pa7TypePtr();
	return stripped->children[0];
}

bool Pa8ProgramSema::IsPointerLike(const Pa7TypePtr& type) const
{
	Pa7TypePtr stripped = StripCV(type);
	return stripped && (stripped->kind == PA7_TYPE_POINTER ||
		stripped->kind == PA7_TYPE_LVALUE_REFERENCE ||
		stripped->kind == PA7_TYPE_RVALUE_REFERENCE);
}

size_t Pa8ProgramSema::FundamentalSize(EFundamentalType type) const
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
	case FT_CHAR:
	case FT_BOOL: return 1;
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T: return 2;
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_WCHAR_T:
	case FT_CHAR32_T:
		return 4;
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_DOUBLE: return 8;
	case FT_FLOAT: return 4;
	case FT_LONG_DOUBLE: return 16;
	case FT_NULLPTR_T: return 8;
	default: return 0;
	}
}

size_t Pa8ProgramSema::FundamentalAlignment(EFundamentalType type) const
{
	return FundamentalSize(type);
}

size_t Pa8ProgramSema::TypeAlignment(const Pa7TypePtr& type)
{
	Pa7TypePtr stripped = StripCV(type);
	if (!stripped)
		throw runtime_error("invalid object type");
	if (stripped->kind == PA7_TYPE_FUNDAMENTAL)
	{
		const size_t result = FundamentalAlignment(stripped->fundamental);
		if (!result)
			throw runtime_error("incomplete object type");
		return result;
	}
	if (stripped->kind == PA7_TYPE_POINTER ||
		stripped->kind == PA7_TYPE_LVALUE_REFERENCE ||
		stripped->kind == PA7_TYPE_RVALUE_REFERENCE)
		return 8;
	if (stripped->kind == PA7_TYPE_ARRAY)
		return TypeAlignment(stripped->children.empty() ? Pa7TypePtr() :
			stripped->children[0]);
	throw runtime_error("function is not an object type");
}

size_t Pa8ProgramSema::TypeSize(const Pa7TypePtr& type)
{
	Pa7TypePtr stripped = StripCV(type);
	if (!stripped)
		throw runtime_error("invalid object type");
	if (stripped->kind == PA7_TYPE_FUNDAMENTAL)
	{
		const size_t result = FundamentalSize(stripped->fundamental);
		if (!result)
			throw runtime_error("incomplete object type");
		return result;
	}
	if (stripped->kind == PA7_TYPE_POINTER ||
		stripped->kind == PA7_TYPE_LVALUE_REFERENCE ||
		stripped->kind == PA7_TYPE_RVALUE_REFERENCE)
		return 8;
	if (stripped->kind == PA7_TYPE_ARRAY)
	{
		if (!stripped->has_bound)
			throw runtime_error("incomplete array type");
		const size_t element_size = TypeSize(stripped->children.empty() ?
			Pa7TypePtr() : stripped->children[0]);
		if (stripped->bound != 0 && element_size >
			numeric_limits<size_t>::max() / stripped->bound)
			throw runtime_error("array is too large");
		return element_size * static_cast<size_t>(stripped->bound);
	}
	throw runtime_error("function is not an object type");
}

Pa8ProgramSema::ResolvedDecl Pa8ProgramSema::ResolveExpression(
	const Pa8Expr& expression) const
{
	ResolvedDecl result;
	if (expression.path.empty() || !expression.lookup_scope)
		return result;
	if (expression.path.size() == 1 && !expression.absolute)
	{
		for (const Pa7Namespace* scope = expression.lookup_scope; scope;
			scope = scope->parent)
		{
			Pa7Decl* found = LookupInNamespace(scope, expression.path[0],
				PA7_FIND_VARIABLE | PA7_FIND_FUNCTION);
			if (found)
			{
				result.declaration = found;
				result.scope = const_cast<Pa7Namespace*>(scope);
				return result;
			}
		}
		return result;
	}

	Pa7Namespace* current = 0;
	size_t index = 0;
	if (expression.absolute)
		current = globals_.empty() ? 0 : globals_[0].get();
	else
	{
		for (const Pa7Namespace* scope = expression.lookup_scope; scope;
			scope = scope->parent)
		{
			Pa7Decl* found = LookupInNamespace(scope, expression.path[0],
				PA7_FIND_NAMESPACE);
			if (found && found->namespace_entity)
			{
				current = found->namespace_entity;
				index = 1;
				break;
			}
		}
	}
	if (!current)
		return result;
	while (index + 1 < expression.path.size())
	{
		Pa7Decl* found = LookupInNamespace(current, expression.path[index],
			PA7_FIND_NAMESPACE);
		if (!found || !found->namespace_entity)
			return result;
		current = found->namespace_entity;
		++index;
	}
	Pa7Decl* found = LookupInNamespace(current, expression.path.back(),
		PA7_FIND_VARIABLE | PA7_FIND_FUNCTION);
	if (found)
	{
		result.declaration = found;
		result.scope = current;
	}
	return result;
}

Pa7Decl* Pa8ProgramSema::LookupInNamespace(const Pa7Namespace* scope,
	const string& name, unsigned filter) const
{
	if (!scope)
		return 0;
	const Pa7Decl* direct = scope->FindDirectOrInline(name, filter);
	if (direct)
		return const_cast<Pa7Decl*>(direct);
	map<const Pa7Namespace*, bool> visited;
	return LookupUsing(scope, name, filter, visited);
}

Pa7Decl* Pa8ProgramSema::LookupUsing(const Pa7Namespace* scope,
	const string& name, unsigned filter,
	map<const Pa7Namespace*, bool>& visited) const
{
	if (!scope || visited[scope])
		return 0;
	visited[scope] = true;
	for (size_t i = 0; i < scope->using_directives.size(); ++i)
	{
		const Pa7Namespace* nominated = scope->using_directives[i];
		if (!nominated)
			continue;
		Pa7Decl* direct = const_cast<Pa7Decl*>(
			nominated->FindDirectOrInline(name, filter));
		if (direct)
			return direct;
		Pa7Decl* transitive = LookupUsing(nominated, name, filter, visited);
		if (transitive)
			return transitive;
	}
	return 0;
}

Pa8Value Pa8ProgramSema::NullValue(const Pa7TypePtr& type) const
{
	Pa8Value result;
	result.type = type;
	result.is_constant = true;
	result.bytes.assign(const_cast<Pa8ProgramSema*>(this)->TypeSize(type), 0);
	return result;
}

Pa8Value Pa8ProgramSema::EvaluateLiteral(const Pa8Expr& expression)
{
	Pa8Value result;
	const Pa6Token& token = expression.literal;
	if (token.kind == PA6_SIMPLE_TOKEN && token.simple_type == KW_TRUE)
	{
		result.type = MakeFundamental(FT_BOOL);
		result.bytes.push_back(1);
		result.is_constant = true;
		return result;
	}
	if (token.kind == PA6_SIMPLE_TOKEN && token.simple_type == KW_FALSE)
	{
		result.type = MakeFundamental(FT_BOOL);
		result.bytes.push_back(0);
		result.is_constant = true;
		return result;
	}
	if (token.kind == PA6_SIMPLE_TOKEN && token.simple_type == KW_NULLPTR)
	{
		result.type = MakeFundamental(FT_NULLPTR_T);
		result.bytes.assign(8, 0);
		result.is_constant = true;
		return result;
	}
	if (!token.lit_scalar)
	{
		if (token.spelling.find('"') == string::npos)
			throw runtime_error("unsupported literal expression");
		result.type = MakeArray(true, 0,
			ApplyCV(PA7_CV_CONST, MakeFundamental(FT_CHAR)));
		result.bytes = DecodeString(token.spelling);
		result.type->bound = result.bytes.size();
		result.is_constant = true;
		result.is_lvalue = true;
		result.is_string_literal = true;
		result.string_symbol = RegisterStringLiteral(expression);
		return result;
	}
	result.type = MakeFundamental(token.lit_type);
	if (IsFloatingType(result.type))
	{
		Numeric number;
		number.valid = true;
		number.floating = true;
		number.floating_value = strtold(token.spelling.c_str(), 0);
		result = EncodeNumeric(number, result.type);
	}
	else
	{
		result.bytes.assign(FundamentalSize(token.lit_type), 0);
		AppendLittleEndian(result.bytes, token.lit_value, result.bytes.size());
		result.is_constant = true;
	}
	return result;
}

Pa8Value Pa8ProgramSema::LoadLvalue(const Pa8Value& source,
	bool require_constant)
{
	if (!source.is_lvalue)
	{
		if (require_constant && !source.is_constant)
			throw runtime_error("expression is not a constant expression");
		return source;
	}
	if (!source.is_constant)
		throw runtime_error("lvalue-to-rvalue conversion is not constant");
	Pa8Value result = source;
	result.is_lvalue = false;
	return result;
}

Pa8Value Pa8ProgramSema::EvaluateVariable(Pa7Variable* variable)
{
	map<const Pa7Variable*, size_t>::const_iterator index =
		variable_entities_.find(variable);
	if (index == variable_entities_.end())
		throw runtime_error("variable is not in the program model");
	Pa8ProgramEntity& entity = entities_[index->second];
	Pa7Variable* definition = 0;
	for (size_t i = 0; i < entity.variables.size(); ++i)
		if (entity.variables[i]->defined)
		{
			definition = entity.variables[i];
			break;
		}
	if (!definition)
		throw runtime_error("variable is odr-used without a definition");
	EvalState& state = variable_states_[definition];
	if (state.complete)
		return state.value;
	if (state.active)
		throw runtime_error("cyclic constant initializer");
	state.active = true;
	Pa8Value value;
	if (definition->initializer_expression)
	{
		Pa8Value source = EvaluateExpression(definition->initializer_expression,
			false);
		value = Convert(source, entity.type, definition->is_constexpr);
	}
	else
		value = NullValue(entity.type);
	value.type = entity.type;
	state.value = value;
	state.complete = true;
	state.active = false;
	definition->initializer.reset(new Pa8Value(value));
	entity.value = value;
	return value;
}

Pa8Value Pa8ProgramSema::EvaluateUnary(const Pa8Expr& expression)
{
	Pa8Value child = EvaluateExpression(expression.left, false);
	if (expression.op == OP_AMP)
	{
		if (!child.is_lvalue && !child.type)
			throw runtime_error("address-of requires an lvalue");
		Pa8Value result;
		result.type = MakePointer(child.type);
		result.bytes.assign(8, 0);
		result.relocs = child.relocs;
		result.is_constant = true;
		return result;
	}
	if (expression.op == OP_STAR)
	{
		child = LoadLvalue(child, false);
		Pa7TypePtr pointed = Pointee(child.type);
		if (!pointed || (child.relocs.empty() && IsZero(child)))
			throw runtime_error("dereference of non-pointer expression");
		Pa8Value result;
		result.type = pointed;
		result.relocs = child.relocs;
		result.is_lvalue = true;
		result.is_constant = false;
		return result;
	}
	child = LoadLvalue(child, false);
	if (!child.is_constant)
		throw runtime_error("non-constant unary expression");
	Numeric number = DecodeNumeric(child);
	if (!number.valid)
		throw runtime_error("unary operator requires an arithmetic operand");
	if (expression.op == OP_LNOT)
	{
		Pa8Value result;
		result.type = MakeFundamental(FT_BOOL);
		result.bytes.push_back((number.floating ? number.floating_value != 0 :
			(number.signed_value != 0 || number.unsigned_value != 0)) ? 0 : 1);
		result.is_constant = true;
		return result;
	}
	if (expression.op == OP_MINUS)
	{
		if (number.floating)
			number.floating_value = -number.floating_value;
		else if (number.signed_value != 0 || number.unsigned_value == 0)
			number.signed_value = -number.signed_value;
		else
			number.unsigned_value = 0 - number.unsigned_value;
	}
	else if (expression.op == OP_COMPL)
	{
		if (number.floating)
			throw runtime_error("complement of floating expression");
		number.unsigned_value = ~ReadLittleEndian(child.bytes);
		number.signed_value = static_cast<long long>(number.unsigned_value);
	}
	return EncodeNumeric(number, child.type);
}

Pa8Value Pa8ProgramSema::EvaluateBinary(const Pa8Expr& expression)
{
	Pa8Value left = LoadLvalue(EvaluateExpression(expression.left, false),
		false);
	Pa8Value right = LoadLvalue(EvaluateExpression(expression.right, false),
		false);
	if (!left.is_constant || !right.is_constant)
		throw runtime_error("non-constant binary expression");
	Numeric a = DecodeNumeric(left);
	Numeric b = DecodeNumeric(right);
	if (!a.valid || !b.valid)
		throw runtime_error("binary operator requires arithmetic operands");
	const ETokenType op = expression.op;
	if (op == OP_LAND || op == OP_LOR || op == OP_EQ || op == OP_NE ||
		op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE)
	{
		long double av = a.floating ? a.floating_value :
			static_cast<long double>(a.signed_value);
		long double bv = b.floating ? b.floating_value :
			static_cast<long double>(b.signed_value);
		bool value = false;
		switch (op)
		{
		case OP_LAND: value = (av != 0) && (bv != 0); break;
		case OP_LOR: value = (av != 0) || (bv != 0); break;
		case OP_EQ: value = av == bv; break;
		case OP_NE: value = av != bv; break;
		case OP_LT: value = av < bv; break;
		case OP_GT: value = av > bv; break;
		case OP_LE: value = av <= bv; break;
		case OP_GE: value = av >= bv; break;
		default: break;
		}
		Pa8Value result;
		result.type = MakeFundamental(FT_BOOL);
		result.bytes.push_back(value ? 1 : 0);
		result.is_constant = true;
		return result;
	}
	Numeric result_number;
	result_number.valid = true;
	result_number.floating = a.floating || b.floating;
	if (result_number.floating)
	{
		const long double av = a.floating ? a.floating_value :
			static_cast<long double>(a.signed_value);
		const long double bv = b.floating ? b.floating_value :
			static_cast<long double>(b.signed_value);
		switch (op)
		{
		case OP_PLUS: result_number.floating_value = av + bv; break;
		case OP_MINUS: result_number.floating_value = av - bv; break;
		case OP_STAR: result_number.floating_value = av * bv; break;
		case OP_DIV: result_number.floating_value = av / bv; break;
		default: throw runtime_error("invalid floating binary operator");
		}
		return EncodeNumeric(result_number, left.type);
	}
	const long long av = a.signed_value;
	const long long bv = b.signed_value;
	switch (op)
	{
	case OP_PLUS: result_number.signed_value = av + bv; break;
	case OP_MINUS: result_number.signed_value = av - bv; break;
	case OP_STAR: result_number.signed_value = av * bv; break;
	case OP_DIV:
		if (bv == 0) throw runtime_error("division by zero");
		result_number.signed_value = av / bv;
		break;
	case OP_MOD:
		if (bv == 0) throw runtime_error("modulo by zero");
		result_number.signed_value = av % bv;
		break;
	case OP_LSHIFT: result_number.signed_value = av << bv; break;
	case OP_RSHIFT: result_number.signed_value = av >> bv; break;
	case OP_BOR: result_number.signed_value = av | bv; break;
	case OP_XOR: result_number.signed_value = av ^ bv; break;
	case OP_AMP: result_number.signed_value = av & bv; break;
	default: throw runtime_error("invalid integer binary operator");
	}
	return EncodeNumeric(result_number, left.type);
}

Pa8Value Pa8ProgramSema::EvaluateExpression(const Pa8ExprPtr& expression,
	bool require_constant)
{
	if (!expression)
		throw runtime_error("missing expression");
	Pa8Value result;
	switch (expression->kind)
	{
	case PA8_EXPR_LITERAL:
		result = EvaluateLiteral(*expression);
		break;
	case PA8_EXPR_IDENTIFIER:
	{
		ResolvedDecl resolved = ResolveExpression(*expression);
		if (!resolved.declaration)
			throw runtime_error("identifier was not found");
		if (resolved.declaration->kind == PA7_DECL_VARIABLE &&
			resolved.declaration->variable)
		{
			Pa7Variable* variable = resolved.declaration->variable.get();
			map<const Pa7Variable*, size_t>::const_iterator index =
				variable_entities_.find(variable);
			if (index == variable_entities_.end())
				throw runtime_error("identifier is not a program variable");
			Pa8ProgramEntity& entity = entities_[index->second];
			Pa7TypePtr variable_type = entity.type ? entity.type : variable->type;
			if (IsConstType(variable_type) && entity.defined)
			{
				result = EvaluateVariable(variable);
				result.type = variable_type;
			}
			else
			{
				result.type = variable_type;
				result.is_lvalue = true;
				result.relocs.push_back(Pa8Relocation(0,
					SymbolFor(variable), 0));
			}
		}
		else if (resolved.declaration->kind == PA7_DECL_FUNCTION &&
			resolved.declaration->function)
		{
			Pa7Function* function = resolved.declaration->function.get();
			result.type = function->type;
			result.is_lvalue = true;
			result.is_constant = true;
			result.relocs.push_back(Pa8Relocation(0,
				SymbolFor(function), 0));
		}
		else
			throw runtime_error("identifier is not an expression");
		break;
	}
	case PA8_EXPR_UNARY:
		result = EvaluateUnary(*expression);
		break;
	case PA8_EXPR_BINARY:
		result = EvaluateBinary(*expression);
		break;
	case PA8_EXPR_CONDITIONAL:
	{
		Pa8Value condition = LoadLvalue(EvaluateExpression(expression->left,
			false), false);
		if (!condition.is_constant)
			throw runtime_error("non-constant conditional expression");
		Numeric number = DecodeNumeric(condition);
		result = EvaluateExpression(number.floating ?
			(number.floating_value != 0 ? expression->right : expression->third) :
			((number.signed_value != 0 || number.unsigned_value != 0) ?
				expression->right : expression->third), false);
		break;
	}
	}
	if (require_constant && !result.is_constant)
		throw runtime_error("expression is not a constant expression");
	expression->annotated_type = result.type;
	expression->lvalue = result.is_lvalue;
	return result;
}

Pa8Value Pa8ProgramSema::EncodeNumeric(const Numeric& number,
	const Pa7TypePtr& target)
{
	Pa8Value result;
	result.type = target;
	Pa7TypePtr stripped = StripCV(target);
	if (!stripped || stripped->kind != PA7_TYPE_FUNDAMENTAL)
		throw runtime_error("numeric value has non-arithmetic target");
	const EFundamentalType type = stripped->fundamental;
	const size_t width = FundamentalSize(type);
	if (!width || type == FT_NULLPTR_T || type == FT_VOID)
		throw runtime_error("invalid numeric target");
	if (IsFloatingFundamental(type))
	{
		const long double value = number.floating ? number.floating_value :
			static_cast<long double>(number.signed_value);
		result.bytes.assign(width, 0);
		if (type == FT_FLOAT)
		{
			const float converted = static_cast<float>(value);
			memcpy(result.bytes.data(), &converted, sizeof(converted));
		}
		else if (type == FT_DOUBLE)
		{
			const double converted = static_cast<double>(value);
			memcpy(result.bytes.data(), &converted, sizeof(converted));
		}
		else
		{
			const long double converted = value;
			memcpy(result.bytes.data(), &converted,
				width < sizeof(converted) ? width : sizeof(converted));
		}
	}
	else
	{
		uint64_t raw;
		if (number.floating)
			raw = static_cast<uint64_t>(number.floating_value);
		else if (type == FT_BOOL)
			raw = (number.signed_value != 0 || number.unsigned_value != 0) ? 1 : 0;
		else if (type == FT_SIGNED_CHAR || type == FT_SHORT_INT ||
			type == FT_INT || type == FT_LONG_INT || type == FT_LONG_LONG_INT ||
			type == FT_CHAR || type == FT_CHAR16_T || type == FT_CHAR32_T ||
			type == FT_WCHAR_T)
			raw = static_cast<uint64_t>(number.signed_value);
		else
			raw = number.unsigned_value;
		result.bytes.assign(width, 0);
		AppendLittleEndian(result.bytes, raw, width);
	}
	result.is_constant = number.valid;
	return result;
}

Pa8ProgramSema::Numeric Pa8ProgramSema::DecodeNumeric(
	const Pa8Value& value) const
{
	Numeric result;
	Pa7TypePtr type = StripCV(value.type);
	if (!type || type->kind != PA7_TYPE_FUNDAMENTAL)
		return result;
	if (IsFloatingFundamental(type->fundamental))
	{
		result.valid = true;
		result.floating = true;
		if (type->fundamental == FT_FLOAT && value.bytes.size() >= 4)
		{
			float number;
			memcpy(&number, value.bytes.data(), sizeof(number));
			result.floating_value = number;
		}
		else if (type->fundamental == FT_DOUBLE && value.bytes.size() >= 8)
		{
			double number;
			memcpy(&number, value.bytes.data(), sizeof(number));
			result.floating_value = number;
		}
		else if (value.bytes.size() >= sizeof(long double))
		{
			long double number;
			memcpy(&number, value.bytes.data(), sizeof(number));
			result.floating_value = number;
		}
		return result;
	}
	if (!IsIntegralFundamental(type->fundamental) || value.relocs.size() != 0)
		return result;
	result.valid = true;
	const uint64_t raw = ReadLittleEndian(value.bytes);
	if (type->fundamental == FT_BOOL)
	{
		result.signed_value = raw != 0;
		result.unsigned_value = raw != 0;
		return result;
	}
	const bool signed_type = type->fundamental == FT_SIGNED_CHAR ||
		type->fundamental == FT_SHORT_INT || type->fundamental == FT_INT ||
		type->fundamental == FT_LONG_INT || type->fundamental == FT_LONG_LONG_INT ||
		type->fundamental == FT_CHAR || type->fundamental == FT_CHAR16_T ||
		type->fundamental == FT_CHAR32_T || type->fundamental == FT_WCHAR_T;
	if (signed_type && !value.bytes.empty() && value.bytes.size() < 8 &&
		(value.bytes.back() & 0x80) != 0)
	{
		const uint64_t mask = ~uint64_t(0) << (value.bytes.size() * 8);
		result.signed_value = static_cast<long long>(raw | mask);
	}
	else
		result.signed_value = static_cast<long long>(raw);
	result.unsigned_value = raw;
	return result;
}

Pa8Value Pa8ProgramSema::Convert(const Pa8Value& source,
	const Pa7TypePtr& target, bool require_constant)
{
	if (!target)
		throw runtime_error("missing initialization target");
	Pa7TypePtr stripped_target = StripCV(target);
	if (!stripped_target)
		throw runtime_error("invalid initialization target");
	if (stripped_target->kind == PA7_TYPE_FUNDAMENTAL &&
		stripped_target->fundamental == FT_BOOL &&
		IsPointerLike(source.type))
	{
		Pa8Value value = LoadLvalue(source, require_constant);
		if (require_constant && !value.is_constant)
			throw runtime_error("pointer-to-bool conversion is not constant");
		Pa8Value result;
		result.type = target;
		result.bytes.push_back((!value.relocs.empty() || !IsZero(value)) ?
			1 : 0);
		result.is_constant = value.is_constant;
		return result;
	}
	if (stripped_target->kind == PA7_TYPE_LVALUE_REFERENCE ||
		stripped_target->kind == PA7_TYPE_RVALUE_REFERENCE)
	{
		if (stripped_target->kind == PA7_TYPE_RVALUE_REFERENCE &&
			source.is_lvalue)
			throw runtime_error("rvalue reference cannot bind an lvalue");
		Pa7TypePtr target_referred = Pointee(target);
		if (target_referred && !IsConstType(target_referred) &&
			IsConstType(source.type))
			throw runtime_error("reference initialization drops const");
		if (!source.is_lvalue)
			throw runtime_error("temporary reference requires block2 support");
		Pa8Value result;
		result.type = target;
		result.bytes.assign(8, 0);
		result.relocs = source.relocs;
		result.is_constant = source.is_constant;
		if (require_constant && !result.is_constant)
			throw runtime_error("reference initializer is not constant");
		return result;
	}
	if (stripped_target->kind == PA7_TYPE_POINTER)
	{
		Pa8Value value = source;
		Pa7TypePtr source_type = StripCV(value.type);
		if (value.is_string_literal)
		{
			Pa7TypePtr target_element = Pointee(target);
			if (!target_element || !IsConstType(target_element))
				throw runtime_error("string literal cannot initialize mutable pointer");
			Pa8Value result;
			result.type = target;
			result.bytes.assign(8, 0);
			result.relocs.push_back(Pa8Relocation(0, value.string_symbol, 0));
			result.is_constant = true;
			return result;
		}
		if (source_type && source_type->kind == PA7_TYPE_ARRAY &&
			value.is_lvalue)
		{
			Pa8Value result;
			result.type = target;
			result.bytes.assign(8, 0);
			result.relocs = value.relocs;
			result.is_constant = true;
			return result;
		}
		if (source_type && source_type->kind == PA7_TYPE_FUNCTION)
		{
			Pa8Value result;
			result.type = target;
			result.bytes.assign(8, 0);
			result.relocs = value.relocs;
			result.is_constant = true;
			return result;
		}
		if (source_type && source_type->kind == PA7_TYPE_POINTER)
		{
			value = LoadLvalue(value, require_constant);
			if (require_constant && !value.is_constant)
				throw runtime_error("pointer initializer is not constant");
			value.type = target;
			return value;
		}
		if (source_type && source_type->kind == PA7_TYPE_FUNDAMENTAL &&
			source_type->fundamental == FT_NULLPTR_T)
			return NullValue(target);
		value = LoadLvalue(value, require_constant);
		Numeric number = DecodeNumeric(value);
		if (!number.valid || !IsIntegralType(value.type))
			throw runtime_error("invalid pointer initializer");
		if (number.signed_value != 0 || number.unsigned_value != 0)
			throw runtime_error("nonzero integer is not a null pointer constant");
		return NullValue(target);
	}
	if (stripped_target->kind == PA7_TYPE_ARRAY)
	{
		if (!source.is_string_literal)
			throw runtime_error("array initializer is not supported");
		Pa8Value result;
		result.type = target;
		const size_t size = TypeSize(target);
		if (source.bytes.size() > size)
			throw runtime_error("string literal does not fit array");
		result.bytes.assign(size, 0);
		copy(source.bytes.begin(), source.bytes.end(), result.bytes.begin());
		result.is_constant = true;
		return result;
	}
	if (stripped_target->kind != PA7_TYPE_FUNDAMENTAL ||
		stripped_target->fundamental == FT_VOID ||
		stripped_target->fundamental == FT_NULLPTR_T)
		throw runtime_error("invalid scalar initialization target");
	Pa8Value value = LoadLvalue(source, require_constant);
	if (!value.is_constant)
		throw runtime_error("scalar initializer is not constant");
	Numeric number = DecodeNumeric(value);
	if (!number.valid)
		throw runtime_error("scalar initializer is not arithmetic");
	return EncodeNumeric(number, target);
}

bool Pa8ProgramSema::IsZero(const Pa8Value& value) const
{
	if (!value.relocs.empty())
		return false;
	for (size_t i = 0; i < value.bytes.size(); ++i)
		if (value.bytes[i] != 0)
			return false;
	return true;
}

unsigned long long Pa8ProgramSema::ArrayBound(const Pa7TypePtr& type)
{
	if (!type || type->kind != PA7_TYPE_ARRAY || !type->has_bound)
		throw runtime_error("array has no bound");
	if (type->bound_expression)
	{
		Pa8Value value = EvaluateExpression(type->bound_expression, true);
		Numeric number = DecodeNumeric(value);
		if (!number.valid || !IsIntegralType(value.type) ||
			(number.signed_value <= 0 && number.unsigned_value == 0))
			throw runtime_error("array bound is not a positive constant");
		type->bound = number.unsigned_value != 0 ? number.unsigned_value :
			static_cast<unsigned long long>(number.signed_value);
		type->bound_expression.reset();
	}
	if (type->bound == 0)
		throw runtime_error("array bound is zero");
	return type->bound;
}

void Pa8ProgramSema::ResolveArrayBounds(const Pa7TypePtr& type)
{
	if (!type)
		return;
	if (type->kind == PA7_TYPE_ARRAY && type->has_bound)
		ArrayBound(type);
	for (size_t i = 0; i < type->children.size(); ++i)
		ResolveArrayBounds(type->children[i]);
	ResolveArrayBounds(type->return_type);
}

vector<unsigned char> Pa8ProgramSema::DecodeString(const string& spelling) const
{
	const size_t quote = spelling.find('"');
	if (quote == string::npos)
		throw runtime_error("invalid string literal");
	const string prefix = spelling.substr(0, quote);
	const size_t width = prefix == "u" ? 2 :
		(prefix == "U" || prefix == "L" ? 4 : 1);
	const size_t end = spelling.rfind('"');
	if (end <= quote)
		throw runtime_error("invalid string literal");
	vector<uint32_t> codepoints;
	for (size_t i = quote + 1; i < end; ++i)
	{
		unsigned value = static_cast<unsigned char>(spelling[i]);
		if (spelling[i] == '\\' && i + 1 < end)
		{
			const char escaped = spelling[++i];
			switch (escaped)
			{
			case 'a': value = 7; break;
			case 'b': value = 8; break;
			case 'f': value = 12; break;
			case 'n': value = 10; break;
			case 'r': value = 13; break;
			case 't': value = 9; break;
			case 'v': value = 11; break;
			case '\\': value = '\\'; break;
			case '"': value = '"'; break;
			case '\'': value = '\''; break;
			case 'x':
			{
				value = 0;
				while (i + 1 < end && HexValue(spelling[i + 1]) >= 0)
					value = value * 16 +
						static_cast<unsigned>(HexValue(spelling[++i]));
				break;
			}
			default:
				if (escaped >= '0' && escaped <= '7')
				{
					value = escaped - '0';
					for (unsigned count = 0; count < 2 && i + 1 < end &&
						spelling[i + 1] >= '0' && spelling[i + 1] <= '7'; ++count)
						value = value * 8 +
							static_cast<unsigned>(spelling[++i] - '0');
				}
				else
					value = static_cast<unsigned char>(escaped);
				break;
			}
		}
		codepoints.push_back(value);
	}
	codepoints.push_back(0);
	vector<unsigned char> result;
	for (size_t i = 0; i < codepoints.size(); ++i)
	{
		const size_t offset = result.size();
		result.resize(offset + width, 0);
		for (size_t byte = 0; byte < width; ++byte)
			result[offset + byte] = static_cast<unsigned char>(
				codepoints[i] >> (byte * 8));
	}
	return result;
}

string Pa8ProgramSema::RegisterStringLiteral(const Pa8Expr& expression)
{
	map<const Pa8Expr*, string>::const_iterator existing =
		string_symbols_.find(&expression);
	if (existing != string_symbols_.end())
		return existing->second;
	ostringstream symbol;
	symbol << "@string" << next_string_id_++;
	Pa8StringLiteral literal;
	literal.symbol = symbol.str();
	literal.bytes = DecodeString(expression.literal.spelling);
	literal.first_use = expression.token_index;
	strings_.push_back(literal);
	string_symbols_[&expression] = literal.symbol;
	return literal.symbol;
}

void Pa8ProgramSema::Analyze()
{
	entities_.clear();
	strings_.clear();
	entity_by_key_.clear();
	variable_entities_.clear();
	function_entities_.clear();
	variable_states_.clear();
	string_symbols_.clear();
	next_string_id_ = 0;
	for (size_t i = 0; i < globals_.size(); ++i)
		CollectNamespace(globals_[i].get(), i);
	stable_sort(entities_.begin(), entities_.end(),
		[](const Pa8ProgramEntity& first, const Pa8ProgramEntity& second) {
			if (first.first_unit != second.first_unit)
				return first.first_unit < second.first_unit;
			return first.first_order < second.first_order;
		});
	entity_by_key_.clear();
	for (size_t i = 0; i < entities_.size(); ++i)
	{
		entity_by_key_[entities_[i].key] = i;
		for (size_t j = 0; j < entities_[i].variables.size(); ++j)
			variable_entities_[entities_[i].variables[j]] = i;
		for (size_t j = 0; j < entities_[i].functions.size(); ++j)
			function_entities_[entities_[i].functions[j]] = i;
	}
	for (size_t i = 0; i < entities_.size(); ++i)
	{
		if (entities_[i].type)
			ResolveArrayBounds(entities_[i].type);
	}
	for (size_t i = 0; i < entities_.size(); ++i)
		if (!entities_[i].is_function && entities_[i].defined)
			EvaluateVariable(entities_[i].variable);
	for (size_t i = 0; i < globals_.size(); ++i)
		for (size_t j = 0; j < globals_[i]->static_assertions.size(); ++j)
		{
			Pa8Value condition = EvaluateExpression(
				globals_[i]->static_assertions[j], true);
			condition = Convert(condition, MakeFundamental(FT_BOOL), true);
			if (IsZero(condition))
				throw runtime_error("static assertion failed");
		}
}
