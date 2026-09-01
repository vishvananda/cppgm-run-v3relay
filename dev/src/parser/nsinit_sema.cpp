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

Pa8Temporary::Pa8Temporary()
	: symbol(), type(), value()
{
}

Pa8StringLiteral::Pa8StringLiteral()
	: symbol(), bytes(), alignment(1)
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
	: globals_(globals), entities_(), temporaries_(), strings_(), entity_by_key_(),
		linked_variables_(), variable_entities_(), function_entities_(),
		variable_states_(), temporary_by_symbol_(), string_by_symbol_(),
		string_symbols_(), next_temporary_id_(0), next_string_id_(0),
		collect_strings_(true)
{
}

const vector<Pa8ProgramEntity>& Pa8ProgramSema::Entities() const
{
	return entities_;
}

const vector<Pa8Temporary>& Pa8ProgramSema::Temporaries() const
{
	return temporaries_;
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
	vector<size_t>& linked = linked_variables_[key];
	// The reference binds a variable first declared without defining to the
	// first linked entity of the name, while one whose first declaration is
	// a definition binds only to a still-undefined entity and otherwise
	// starts a new one (probes p71/p84/p85): duplicate such definitions
	// coexist in the image.
	size_t index = entities_.size();
	if (variable->defined && variable->initially_defined)
	{
		for (size_t i = 0; i < linked.size(); ++i)
			if (!entities_[linked[i]].defined)
			{
				index = linked[i];
				break;
			}
	}
	else if (!linked.empty())
		index = linked[0];
	if (index == entities_.size())
	{
		Pa8ProgramEntity entity;
		entity.key = linked.empty() ? key :
			key + "|" + to_string(linked.size());
		entity.first_unit = unit;
		entity.first_order = variable->order;
		entities_.push_back(entity);
		entity_by_key_[entities_[index].key] = index;
		linked.push_back(index);
	}
	Pa8ProgramEntity& entity = entities_[index];
	if (unit < entity.first_unit ||
		(unit == entity.first_unit && variable->order < entity.first_order))
	{
		entity.first_unit = unit;
		entity.first_order = variable->order;
	}
	entity.variables.push_back(variable);
	if (!entity.variable)
		entity.variable = variable;
	// 3.5 links declarations by name and linkage only; the reference does
	// not require cross-unit type agreement, and the defining declaration's
	// type governs layout and initialization.
	if (variable->defined)
	{
		if (entity.defined)
			throw runtime_error("more than one variable definition");
		entity.defined = true;
		entity.variable = variable;
		entity.type = variable->type;
	}
	else if (!entity.type)
		entity.type = variable->type;
}

void Pa8ProgramSema::AddFunction(Pa7Function* function, size_t unit)
{
	const string key = MakeFunctionKey(function, unit);
	Pa8ProgramEntity* entity = FindOrCreateEntity(key, true, unit,
		function->order);
	entity->functions.push_back(function);
	if (!entity->function)
		entity->function = function;
	// Cross-unit duplicate function definitions link to the one entity with
	// no diagnostic (probe p83); a same-unit duplicate is rejected by the
	// model when the redeclaration merges.
	if (function->defined)
	{
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

namespace
{

// Resolution runs after the whole translation unit is parsed, so a candidate
// declared at or after the expression's snapshot is not visible to it (3.4).
bool DeclaredBeforeExpression(const Pa7Decl* declaration, size_t epoch)
{
	if (!declaration)
		return false;
	if (declaration->kind == PA7_DECL_VARIABLE && declaration->variable)
		return declaration->variable->order < epoch;
	if (declaration->kind == PA7_DECL_FUNCTION && declaration->function)
		return declaration->function->order < epoch;
	return true;
}

} // namespace

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
			if (found && DeclaredBeforeExpression(found, expression.decl_epoch))
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
	if (found && DeclaredBeforeExpression(found, expression.decl_epoch))
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
		const size_t quote = token.spelling.find('"');
		const string prefix = token.spelling.substr(0, quote);
		const EFundamentalType element_type = prefix == "u" ? FT_CHAR16_T :
			(prefix == "U" ? FT_CHAR32_T :
			(prefix == "L" ? FT_WCHAR_T : FT_CHAR));
		result.bytes = DecodeString(token.spelling);
		const size_t element_width = FundamentalSize(element_type);
		if (!element_width || result.bytes.size() % element_width != 0)
			throw runtime_error("invalid string literal element width");
		result.type = MakeArray(true, result.bytes.size() / element_width,
			ApplyCV(PA7_CV_CONST, MakeFundamental(element_type)));
		result.is_constant = true;
		result.is_lvalue = true;
		result.is_string_literal = true;
		result.string_symbol = RegisterStringLiteral(expression);
		result.address_relocs.push_back(Pa8Relocation(0,
			result.string_symbol, 0));
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
	if (require_constant && !source.is_constant)
		throw runtime_error("lvalue-to-rvalue conversion is not constant");
	// Empty bytes mean no definition supplied a value anywhere in the
	// program, so even relaxed initializer folding cannot read the object.
	if (source.bytes.empty())
		throw runtime_error("value of the object is not known");
	Pa8Value result = source;
	result.is_lvalue = false;
	result.address_relocs.clear();
	return result;
}

Pa8Value Pa8ProgramSema::EvaluateVariable(Pa7Variable* variable)
{
	map<const Pa7Variable*, size_t>::const_iterator index =
		variable_entities_.find(variable);
	if (index == variable_entities_.end())
		throw runtime_error("variable is not in the program model");
	Pa8ProgramEntity& entity = entities_[index->second];
	if (!entity.defined)
		throw runtime_error("variable is used without a definition");
	Pa7Variable* definition = entity.variable;
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
	entity.value = value;
	return value;
}

Pa8Value Pa8ProgramSema::EvaluateReferencedValue(const Pa8Value& reference,
	const Pa7TypePtr& type)
{
	if (reference.relocs.size() != 1)
		return Pa8Value();
	const Pa8Relocation& relocation = reference.relocs[0];
	map<string, size_t>::const_iterator entity_index =
		entity_by_key_.find(relocation.symbol);
	if (entity_index != entity_by_key_.end())
	{
		Pa8ProgramEntity& entity = entities_[entity_index->second];
		if (entity.is_function || !entity.defined || !entity.variable)
			return Pa8Value();
		Pa8Value result = EvaluateVariable(entity.variable);
		// 5.19: reading the object is a constant expression only if it is
		// constexpr or const-qualified; the image value of any defined
		// variable is still known for relaxed initializer folding.
		if (!entity.variable->is_constexpr && !IsConstQualified(entity.type))
			result.is_constant = false;
		result.type = type;
		return result;
	}
	map<string, size_t>::const_iterator temporary =
		temporary_by_symbol_.find(relocation.symbol);
	if (temporary != temporary_by_symbol_.end())
	{
		Pa8Value result = temporaries_[temporary->second].value;
		result.type = type;
		return result;
	}
	map<string, size_t>::const_iterator literal =
		string_by_symbol_.find(relocation.symbol);
	if (literal != string_by_symbol_.end())
	{
		Pa8Value result;
		result.type = type;
		result.bytes = strings_[literal->second].bytes;
		result.is_constant = true;
		result.is_string_literal = true;
		result.string_symbol = strings_[literal->second].symbol;
		return result;
	}
	return Pa8Value();
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
			Pa7TypePtr stripped_variable_type = StripCV(variable_type);
			if (stripped_variable_type &&
				(stripped_variable_type->kind == PA7_TYPE_LVALUE_REFERENCE ||
				 stripped_variable_type->kind == PA7_TYPE_RVALUE_REFERENCE))
			{
				Pa8Value reference = EvaluateVariable(variable);
				Pa7TypePtr referred_type = Pointee(variable_type);
				if (!referred_type || reference.relocs.empty())
					throw runtime_error("reference has no bound object");
				result.type = referred_type;
				result.is_lvalue = true;
				result.address_relocs = reference.relocs;
				Pa8Value referred = EvaluateReferencedValue(reference,
					referred_type);
				if (referred.type)
				{
					result.bytes = referred.bytes;
					result.relocs = referred.relocs;
					result.is_constant = referred.is_constant;
				}
			}
			else
			{
				result.type = variable_type;
				result.is_lvalue = true;
				result.address_relocs.push_back(Pa8Relocation(0,
					SymbolFor(variable), 0));
				// The image value of every defined variable is known during
				// translation; 5.19 constancy additionally requires constexpr
				// or a const-qualified object type.
				if (entity.defined)
				{
					Pa8Value value = EvaluateVariable(variable);
					result.bytes = value.bytes;
					result.relocs = value.relocs;
					result.is_constant = value.is_constant &&
						(variable->is_constexpr ||
						 IsConstQualified(variable_type));
				}
			}
		}
		else if (resolved.declaration->kind == PA7_DECL_FUNCTION &&
			resolved.declaration->function)
		{
			// The reference rejects an overloaded name in an expression even
			// when only one overload would match the initialization target.
			if (resolved.declaration->function_overloads.size() > 1)
				throw runtime_error("overloaded function name in expression");
			Pa7Function* function = resolved.declaration->function.get();
			result.type = function->type;
			result.is_lvalue = true;
			result.is_constant = true;
			result.address_relocs.push_back(Pa8Relocation(0,
				SymbolFor(function), 0));
		}
		else
			throw runtime_error("identifier is not an expression");
		break;
	}
	}
	if (require_constant && !result.is_constant)
		throw runtime_error("expression is not a constant expression");
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
	Pa7TypePtr stripped_source = StripCV(source.type);
	const bool source_decays = stripped_source &&
		(stripped_source->kind == PA7_TYPE_ARRAY ||
		 stripped_source->kind == PA7_TYPE_FUNCTION) &&
		(source.is_lvalue || source.is_string_literal);
	if (stripped_target->kind == PA7_TYPE_FUNDAMENTAL &&
		stripped_target->fundamental == FT_BOOL &&
		(IsPointerLike(source.type) || source_decays))
	{
		Pa8Value result;
		result.type = target;
		if (source_decays)
		{
			// Array-to-pointer or function-to-pointer decay yields the
			// object's address, which is never null.
			result.bytes.push_back(1);
			result.is_constant = source.is_constant;
			return result;
		}
		Pa8Value value = LoadLvalue(source, require_constant);
		// The reference accepts a pointer in a boolean context only when its
		// value is a constant expression (probes p31/p47).
		if (!value.is_constant)
			throw runtime_error("pointer-to-bool conversion is not constant");
		result.bytes.push_back((!value.relocs.empty() || !IsZero(value)) ?
			1 : 0);
		result.is_constant = true;
		return result;
	}
	if (stripped_target->kind == PA7_TYPE_LVALUE_REFERENCE ||
		stripped_target->kind == PA7_TYPE_RVALUE_REFERENCE)
	{
		Pa7TypePtr target_referred = Pointee(target);
		if (!target_referred || !source.type)
			throw runtime_error("invalid reference target");
		if (stripped_target->kind == PA7_TYPE_RVALUE_REFERENCE &&
			source.is_lvalue)
			throw runtime_error("rvalue reference cannot bind an lvalue");
		if (source.is_lvalue)
		{
			if (!SameType(StripCV(source.type), StripCV(target_referred)))
				throw runtime_error("reference initializer type mismatch");
			if (!IsConstType(target_referred) &&
				IsConstType(source.type))
				throw runtime_error("reference initialization drops const");
			if (source.address_relocs.empty())
				throw runtime_error("reference initializer has no address");
			Pa8Value result;
			result.type = target;
			result.bytes.assign(8, 0);
			result.relocs = source.address_relocs;
			result.is_constant = true;
			return result;
		}
		if (stripped_target->kind == PA7_TYPE_LVALUE_REFERENCE &&
			!IsConstType(target_referred))
			throw runtime_error("non-const lvalue reference needs an lvalue");
		Pa8Value temporary_value = Convert(source, target_referred, true);
		const string temporary = RegisterTemporary(target_referred,
			temporary_value);
		Pa8Value result;
		result.type = target;
		result.bytes.assign(8, 0);
		result.relocs.push_back(Pa8Relocation(0, temporary, 0));
		result.is_constant = true;
		return result;
	}
	if (stripped_target->kind == PA7_TYPE_POINTER)
	{
		Pa8Value value = source;
		Pa7TypePtr source_type = stripped_source;
		if (value.is_string_literal)
		{
			// 4.2p2: only the literal's own element type (made const) is a
			// valid pointee for string-literal decay.
			Pa7TypePtr target_element = Pointee(target);
			Pa7TypePtr source_element = source_type &&
				!source_type->children.empty() ?
				StripCV(source_type->children[0]) : Pa7TypePtr();
			if (!target_element || !IsConstType(target_element))
				throw runtime_error("string literal cannot initialize mutable pointer");
			if (!SameType(StripCV(target_element), source_element))
				throw runtime_error("string literal element type does not agree");
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
			if (source_type->children.size() != 1 ||
				!PointerConvertible(MakePointer(source_type->children[0]),
					target))
				throw runtime_error("array element does not match pointer target");
			Pa8Value result;
			result.type = target;
			result.bytes.assign(8, 0);
			result.relocs = value.address_relocs;
			result.is_constant = true;
			return result;
		}
		if (source_type && source_type->kind == PA7_TYPE_FUNCTION)
		{
			if (!SameType(StripCV(Pointee(target)), source_type))
				throw runtime_error("function type does not match pointer target");
			Pa8Value result;
			result.type = target;
			result.bytes.assign(8, 0);
			result.relocs = value.address_relocs;
			result.is_constant = true;
			return result;
		}
		if (source_type && source_type->kind == PA7_TYPE_POINTER)
		{
			if (!PointerConvertible(value.type, target))
				throw runtime_error("pointer conversion drops qualifiers or changes type");
			if (value.is_lvalue)
			{
				// The reference realizes a pointer copied from a pointer
				// lvalue as the source object's address (probes p60/p76).
				if (value.address_relocs.empty())
					throw runtime_error("pointer initializer has no address");
				Pa8Value result;
				result.type = target;
				result.bytes.assign(8, 0);
				result.relocs = value.address_relocs;
				result.is_constant = true;
				return result;
			}
			value = LoadLvalue(value, require_constant);
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
		// 4.10p1: only a constant expression of integer type that evaluates
		// to zero is a null pointer constant.
		if (!value.is_constant)
			throw runtime_error("integer-to-pointer initializer is not constant");
		if (number.signed_value != 0 || number.unsigned_value != 0)
			throw runtime_error("nonzero integer is not a null pointer constant");
		return NullValue(target);
	}
	if (stripped_target->kind == PA7_TYPE_ARRAY)
	{
		if (!source.is_string_literal)
			throw runtime_error("array initializer is not supported");
		if (stripped_target->children.size() != 1 ||
			!stripped_source || stripped_source->kind != PA7_TYPE_ARRAY ||
			stripped_source->children.size() != 1)
			throw runtime_error("invalid string literal array type");
		// 8.5.2p1: an ordinary string literal initializes arrays of char,
		// signed char, or unsigned char; u/U/L literals initialize only
		// their own element type.
		Pa7TypePtr source_element = StripCV(stripped_source->children[0]);
		Pa7TypePtr target_element = StripCV(stripped_target->children[0]);
		if (!source_element || !target_element ||
			source_element->kind != PA7_TYPE_FUNDAMENTAL ||
			target_element->kind != PA7_TYPE_FUNDAMENTAL)
			throw runtime_error("invalid string literal array type");
		const EFundamentalType source_kind = source_element->fundamental;
		const EFundamentalType target_kind = target_element->fundamental;
		const bool element_agrees = source_kind == FT_CHAR ?
			(target_kind == FT_CHAR || target_kind == FT_SIGNED_CHAR ||
			 target_kind == FT_UNSIGNED_CHAR) :
			target_kind == source_kind;
		if (!element_agrees)
			throw runtime_error("string literal element type does not agree");
		const size_t target_element_size = TypeSize(
			stripped_target->children[0]);
		if (!target_element_size ||
			source.bytes.size() % target_element_size != 0)
			throw runtime_error("string literal element size does not agree");
		if (!stripped_target->has_bound)
		{
			stripped_target->has_bound = true;
			stripped_target->bound = source.bytes.size() /
				target_element_size;
		}
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
	// The mock image has no runtime, so every defined variable's value is
	// known during translation and arithmetic initializers fold even when
	// they are not 5.19 constant expressions (probes p1/p48/p55).
	Pa8Value value = LoadLvalue(source, require_constant);
	Numeric number = DecodeNumeric(value);
	if (!number.valid)
		throw runtime_error("scalar initializer is not arithmetic");
	Pa8Value result = EncodeNumeric(number, target);
	result.is_constant = value.is_constant;
	return result;
}

// 4.4: T1* converts to T2* when both point at the same type modulo
// cv-qualifiers, no qualifier is dropped at any level, and any level that
// adds qualifiers has const at every level above it.
bool Pa8ProgramSema::PointerConvertible(const Pa7TypePtr& source,
	const Pa7TypePtr& target) const
{
	Pa7TypePtr from = StripCV(source);
	Pa7TypePtr to = StripCV(target);
	bool const_above = true;
	while (from && to && from->kind == PA7_TYPE_POINTER &&
		to->kind == PA7_TYPE_POINTER &&
		from->children.size() == 1 && to->children.size() == 1)
	{
		const Pa7TypePtr& from_pointee = from->children[0];
		const Pa7TypePtr& to_pointee = to->children[0];
		const unsigned from_cv = from_pointee &&
			from_pointee->kind == PA7_TYPE_CV ? from_pointee->cv : 0;
		const unsigned to_cv = to_pointee &&
			to_pointee->kind == PA7_TYPE_CV ? to_pointee->cv : 0;
		if ((from_cv & ~to_cv) != 0)
			return false;
		if (from_cv != to_cv && !const_above)
			return false;
		const_above = (to_cv & PA7_CV_CONST) != 0;
		from = StripCV(from_pointee);
		to = StripCV(to_pointee);
	}
	return SameType(from, to);
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

string Pa8ProgramSema::RegisterTemporary(const Pa7TypePtr& type,
	const Pa8Value& source)
{
	ostringstream symbol;
	symbol << "@temporary" << next_temporary_id_++;
	Pa8Temporary temporary;
	temporary.symbol = symbol.str();
	temporary.type = type;
	temporary.value = source;
	temporary.value.type = type;
	temporary.value.is_lvalue = false;
	temporary.value.address_relocs.clear();
	temporaries_.push_back(temporary);
	temporary_by_symbol_[temporary.symbol] = temporaries_.size() - 1;
	return temporary.symbol;
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
	// static_assert operands still decay and compare non-null, but their
	// literal objects are not part of the image (probe p82); the dangling
	// symbol resolves to address 0.
	if (!collect_strings_)
		return symbol.str();
	Pa8StringLiteral literal;
	literal.symbol = symbol.str();
	literal.bytes = DecodeString(expression.literal.spelling);
	const size_t quote = expression.literal.spelling.find('"');
	const string prefix = expression.literal.spelling.substr(0, quote);
	literal.alignment = prefix == "u" ? 2 :
		(prefix == "U" || prefix == "L" ? 4 : 1);
	strings_.push_back(literal);
	string_by_symbol_[literal.symbol] = strings_.size() - 1;
	string_symbols_[&expression] = literal.symbol;
	return literal.symbol;
}

// Every declared type of one variable (one entity within one translation
// unit) must agree once expression-spelled array bounds are evaluated; the
// parser can only compare literal bounds (probes p41/p42/p78).
void Pa8ProgramSema::CheckDeclaredTypeAgreement()
{
	for (size_t i = 0; i < entities_.size(); ++i)
	{
		if (entities_[i].is_function)
			continue;
		for (size_t j = 0; j < entities_[i].variables.size(); ++j)
		{
			Pa7Variable* variable = entities_[i].variables[j];
			for (size_t k = 0; k < variable->declared_types.size(); ++k)
				ResolveArrayBounds(variable->declared_types[k]);
			for (size_t k = 1; k < variable->declared_types.size(); ++k)
				if (!RedeclarationTypesAgree(variable->declared_types[0],
					variable->declared_types[k]))
					throw runtime_error(
						"variable type does not agree across redeclarations");
		}
	}
}

void Pa8ProgramSema::Analyze()
{
	entities_.clear();
	temporaries_.clear();
	strings_.clear();
	entity_by_key_.clear();
	linked_variables_.clear();
	variable_entities_.clear();
	function_entities_.clear();
	variable_states_.clear();
	temporary_by_symbol_.clear();
	string_by_symbol_.clear();
	string_symbols_.clear();
	next_temporary_id_ = 0;
	next_string_id_ = 0;
	collect_strings_ = true;
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
	CheckDeclaredTypeAgreement();
	for (size_t i = 0; i < entities_.size(); ++i)
		if (!entities_[i].is_function && entities_[i].defined)
			EvaluateVariable(entities_[i].variable);
	collect_strings_ = false;
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
