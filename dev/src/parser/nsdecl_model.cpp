#include "nsdecl_model.h"

#include <sstream>
#include <stdexcept>

using namespace std;

namespace
{

Pa7TypePtr MakeCV(unsigned cv, const Pa7TypePtr& type)
{
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_CV;
	result->cv = cv;
	result->children.push_back(type);
	return result;
}

Pa7TypePtr StripCV(const Pa7TypePtr& type)
{
	Pa7TypePtr result = type;
	while (result && result->kind == PA7_TYPE_CV &&
		!result->children.empty())
		result = result->children[0];
	return result;
}

bool IsReferenceKind(Pa7TypeKind kind)
{
	return kind == PA7_TYPE_LVALUE_REFERENCE ||
		kind == PA7_TYPE_RVALUE_REFERENCE;
}

// A bound spelled as a non-literal constant expression has value 0 until the
// semantic pass evaluates it; two such bounds compare equal only if they are
// the same expression.  Known (nonzero) bounds compare by value.
bool SameArrayBound(const Pa7TypePtr& left, const Pa7TypePtr& right)
{
	if (left->bound != 0 && right->bound != 0)
		return left->bound == right->bound;
	return left->bound_expression == right->bound_expression;
}

bool SameTypeImpl(const Pa7TypePtr& left, const Pa7TypePtr& right)
{
	if (left == right)
		return true;
	if (!left || !right || left->kind != right->kind)
		return false;

	switch (left->kind)
	{
	case PA7_TYPE_FUNDAMENTAL:
		return left->fundamental == right->fundamental;
	case PA7_TYPE_CV:
		return left->cv == right->cv && left->children.size() == 1 &&
			right->children.size() == 1 &&
			SameTypeImpl(left->children[0], right->children[0]);
	case PA7_TYPE_POINTER:
	case PA7_TYPE_LVALUE_REFERENCE:
	case PA7_TYPE_RVALUE_REFERENCE:
		return left->children.size() == 1 && right->children.size() == 1 &&
			SameTypeImpl(left->children[0], right->children[0]);
	case PA7_TYPE_ARRAY:
		return left->has_bound == right->has_bound &&
			(!left->has_bound || SameArrayBound(left, right)) &&
			left->children.size() == 1 && right->children.size() == 1 &&
			SameTypeImpl(left->children[0], right->children[0]);
	case PA7_TYPE_FUNCTION:
		if (left->varargs != right->varargs ||
			left->children.size() != right->children.size() ||
			!SameTypeImpl(left->return_type, right->return_type))
			return false;
		for (size_t i = 0; i < left->children.size(); ++i)
			if (!SameTypeImpl(left->children[i], right->children[i]))
				return false;
		return true;
	}
	return false;
}

Pa7TypePtr MergeTypeImpl(const Pa7TypePtr& first, const Pa7TypePtr& second)
{
	if (!first)
		return second;
	if (!second || SameTypeImpl(first, second))
		return first;
	if (first->kind != second->kind)
		return first;

	switch (first->kind)
	{
	case PA7_TYPE_ARRAY:
	{
		const bool has_bound = first->has_bound || second->has_bound;
		const Pa8ExprPtr bound_expression = first->bound_expression ?
			first->bound_expression : second->bound_expression;
		const unsigned long long bound = bound_expression ? 0 :
			(first->has_bound ? first->bound : second->bound);
		Pa7TypePtr first_element = first->children.empty() ? Pa7TypePtr() :
			first->children[0];
		Pa7TypePtr second_element = second->children.empty() ? Pa7TypePtr() :
			second->children[0];
		return MakeArray(has_bound, bound,
			MergeTypeImpl(first_element, second_element), bound_expression);
	}
	case PA7_TYPE_CV:
		if (first->cv == second->cv && first->children.size() == 1 &&
			second->children.size() == 1)
			return ApplyCV(first->cv,
				MergeTypeImpl(first->children[0], second->children[0]));
		return first;
	case PA7_TYPE_POINTER:
	case PA7_TYPE_LVALUE_REFERENCE:
	case PA7_TYPE_RVALUE_REFERENCE:
		if (first->children.size() == 1 && second->children.size() == 1)
		{
			Pa7TypePtr child = MergeTypeImpl(first->children[0],
				second->children[0]);
			if (first->kind == PA7_TYPE_POINTER)
				return MakePointer(child);
			return MakeReference(first->kind == PA7_TYPE_RVALUE_REFERENCE,
				child);
		}
		return first;
	default:
		return first;
	}
}

// 3.5: a function signature is its name and parameter types.  Two same-name
// declarations whose parameter lists agree name one entity, so any remaining
// type difference (the return type) is a redeclaration conflict, not an
// overload.
bool SameSignature(const Pa7TypePtr& first, const Pa7TypePtr& second)
{
	Pa7TypePtr left = StripCV(first);
	Pa7TypePtr right = StripCV(second);
	if (!left || !right || left->kind != PA7_TYPE_FUNCTION ||
		right->kind != PA7_TYPE_FUNCTION ||
		left->varargs != right->varargs ||
		left->children.size() != right->children.size())
		return false;
	for (size_t i = 0; i < left->children.size(); ++i)
		if (!SameTypeImpl(left->children[i], right->children[i]))
			return false;
	return true;
}

void PrintNamespace(ostream& out, const Pa7Namespace& ns, bool global)
{
	if (global || ns.unnamed)
		out << "start unnamed namespace" << endl;
	else
		out << "start namespace " << ns.name << endl;
	if (ns.inline_namespace)
		out << "inline namespace" << endl;

	for (size_t i = 0; i < ns.variables.size(); ++i)
		out << "variable " << ns.variables[i]->name << " "
			<< DescribeType(ns.variables[i]->type) << endl;
	for (size_t i = 0; i < ns.functions.size(); ++i)
		out << "function " << ns.functions[i]->name << " "
			<< DescribeType(ns.functions[i]->type) << endl;
	for (size_t i = 0; i < ns.namespaces.size(); ++i)
		PrintNamespace(out, *ns.namespaces[i], false);

	out << "end namespace" << endl;
}

} // namespace

bool RedeclarationTypesAgree(const Pa7TypePtr& first, const Pa7TypePtr& second)
{
	if (SameType(first, second))
		return true;
	if (!first || !second || first->kind != second->kind)
		return false;
	if (first->kind == PA7_TYPE_ARRAY &&
		first->children.size() == 1 && second->children.size() == 1)
	{
		// Bounds still awaiting constant evaluation (value 0 with a retained
		// expression) are accepted here; the semantic pass re-checks every
		// declared type once bounds are evaluated.
		return (!first->has_bound || !second->has_bound ||
			first->bound == 0 || second->bound == 0 ||
			first->bound == second->bound) &&
			RedeclarationTypesAgree(first->children[0], second->children[0]);
	}
	return false;
}

namespace
{

void CheckDeclarationAttributes(const Pa7Variable& prior,
	const Pa7DeclAttributes& next, bool strict)
{
	if (!strict)
		return;
	const bool prior_static = (prior.storage & PA7_STORAGE_STATIC) != 0;
	const bool next_static = (next.storage & PA7_STORAGE_STATIC) != 0;
	if (prior_static != next_static && (prior_static || next_static))
		throw runtime_error("static storage does not agree across redeclarations");
	const bool prior_thread =
		(prior.storage & PA7_STORAGE_THREAD_LOCAL) != 0;
	const bool next_thread =
		(next.storage & PA7_STORAGE_THREAD_LOCAL) != 0;
	if (prior_thread != next_thread)
		throw runtime_error("thread_local does not agree across redeclarations");
	if (prior.linkage != PA7_LINKAGE_UNSPECIFIED &&
		next.linkage_explicit && prior.linkage != next.linkage)
		throw runtime_error("linkage does not agree across redeclarations");
	if (prior.defined && next.defined)
		throw runtime_error("more than one definition");
}

void ApplyVariableAttributes(Pa7Variable& variable,
	const Pa7DeclAttributes& attributes)
{
	if (attributes.storage != PA7_STORAGE_NONE || variable.storage ==
		PA7_STORAGE_NONE)
		variable.storage = attributes.storage;
	if (attributes.linkage != PA7_LINKAGE_UNSPECIFIED)
		variable.linkage = attributes.linkage;
	variable.is_const = variable.is_const || attributes.is_const;
	variable.is_constexpr = variable.is_constexpr || attributes.is_constexpr;
	variable.defined = variable.defined || attributes.defined;
}

void CheckDeclarationAttributes(const Pa7Function& prior,
	const Pa7DeclAttributes& next, bool strict)
{
	if (!strict)
		return;
	const bool prior_static = (prior.storage & PA7_STORAGE_STATIC) != 0;
	const bool next_static = (next.storage & PA7_STORAGE_STATIC) != 0;
	if (prior_static != next_static && (prior_static || next_static))
		throw runtime_error("static storage does not agree across redeclarations");
	const bool prior_thread =
		(prior.storage & PA7_STORAGE_THREAD_LOCAL) != 0;
	const bool next_thread =
		(next.storage & PA7_STORAGE_THREAD_LOCAL) != 0;
	if (prior_thread != next_thread)
		throw runtime_error("thread_local does not agree across redeclarations");
	if (prior.linkage != PA7_LINKAGE_UNSPECIFIED &&
		next.linkage_explicit && prior.linkage != next.linkage)
		throw runtime_error("linkage does not agree across redeclarations");
	// 3.2p1: no translation unit contains more than one definition; an
	// inline function may be redefined in another unit, never in the same
	// one (probe p86).
	if (prior.defined && next.defined)
		throw runtime_error("more than one definition");
}

void ApplyFunctionAttributes(Pa7Function& function,
	const Pa7DeclAttributes& attributes)
{
	if (attributes.storage != PA7_STORAGE_NONE || function.storage ==
		PA7_STORAGE_NONE)
		function.storage = attributes.storage;
	if (attributes.linkage != PA7_LINKAGE_UNSPECIFIED)
		function.linkage = attributes.linkage;
	function.is_inline = function.is_inline || attributes.is_inline;
	function.defined = function.defined || attributes.defined;
}

} // namespace

Pa7Type::Pa7Type()
	: kind(PA7_TYPE_FUNDAMENTAL), fundamental(FT_INT), cv(PA7_CV_NONE),
		has_bound(false), bound(0), bound_expression(), varargs(false),
		return_type()
{
}

Pa7TypePtr MakeFundamental(EFundamentalType type)
{
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_FUNDAMENTAL;
	result->fundamental = type;
	return result;
}

Pa7TypePtr ApplyCV(unsigned cv, const Pa7TypePtr& type)
{
	if (!type || cv == PA7_CV_NONE)
		return type;
	if (IsReferenceKind(type->kind))
		return type;
	if (type->kind == PA7_TYPE_ARRAY && type->children.size() == 1)
		return MakeArray(type->has_bound, type->bound,
			ApplyCV(cv, type->children[0]), type->bound_expression);
	if (type->kind == PA7_TYPE_CV && type->children.size() == 1)
		return ApplyCV(type->cv | cv, type->children[0]);
	return MakeCV(cv, type);
}

Pa7TypePtr MakePointer(const Pa7TypePtr& inner)
{
	Pa7TypePtr stripped = StripCV(inner);
	if (stripped && IsReferenceKind(stripped->kind))
		throw runtime_error("pointer to reference is not a type");
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_POINTER;
	result->children.push_back(inner);
	return result;
}

Pa7TypePtr MakeReference(bool is_rvalue, const Pa7TypePtr& inner)
{
	Pa7TypePtr stripped = StripCV(inner);
	if (stripped && stripped->kind == PA7_TYPE_FUNDAMENTAL &&
		stripped->fundamental == FT_VOID)
		throw runtime_error("reference to void is not a type");
	if (inner && IsReferenceKind(inner->kind) &&
		inner->children.size() == 1)
		return MakeReference(is_rvalue &&
			inner->kind == PA7_TYPE_RVALUE_REFERENCE,
			inner->children[0]);

	Pa7TypePtr result(new Pa7Type);
	result->kind = is_rvalue ? PA7_TYPE_RVALUE_REFERENCE :
		PA7_TYPE_LVALUE_REFERENCE;
	result->children.push_back(inner);
	return result;
}

Pa7TypePtr MakeArray(bool has_bound, unsigned long long bound,
	const Pa7TypePtr& element, const Pa8ExprPtr& bound_expression)
{
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_ARRAY;
	result->has_bound = has_bound;
	result->bound = bound;
	result->bound_expression = bound_expression;
	result->children.push_back(element);
	return result;
}

Pa7TypePtr MakeFunction(const vector<Pa7TypePtr>& params, bool varargs,
	const Pa7TypePtr& ret)
{
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_FUNCTION;
	result->children = params;
	result->varargs = varargs;
	result->return_type = ret;
	return result;
}

string DescribeType(const Pa7TypePtr& type)
{
	if (!type)
		return "<invalid>";

	ostringstream out;
	switch (type->kind)
	{
	case PA7_TYPE_FUNDAMENTAL:
	{
		map<EFundamentalType, string>::const_iterator it =
			FundamentalTypeToStringMap.find(type->fundamental);
		return it == FundamentalTypeToStringMap.end() ? "<fundamental>" :
			it->second;
	}
	case PA7_TYPE_CV:
		if (type->cv & PA7_CV_CONST)
			out << "const ";
		if (type->cv & PA7_CV_VOLATILE)
			out << "volatile ";
		return out.str() + DescribeType(type->children.empty() ?
			Pa7TypePtr() : type->children[0]);
	case PA7_TYPE_POINTER:
		return "pointer to " + DescribeType(type->children.empty() ?
			Pa7TypePtr() : type->children[0]);
	case PA7_TYPE_LVALUE_REFERENCE:
		return "lvalue-reference to " + DescribeType(type->children.empty() ?
			Pa7TypePtr() : type->children[0]);
	case PA7_TYPE_RVALUE_REFERENCE:
		return "rvalue-reference to " + DescribeType(type->children.empty() ?
			Pa7TypePtr() : type->children[0]);
	case PA7_TYPE_ARRAY:
		out << "array of ";
		if (type->has_bound)
			out << type->bound << " ";
		else
			out << "unknown bound of ";
		return out.str() + DescribeType(type->children.empty() ?
			Pa7TypePtr() : type->children[0]);
	case PA7_TYPE_FUNCTION:
		out << "function of (";
		for (size_t i = 0; i < type->children.size(); ++i)
		{
			if (i != 0)
				out << ", ";
			out << DescribeType(type->children[i]);
		}
		if (type->varargs)
		{
			if (!type->children.empty())
				out << ", ";
			out << "...";
		}
		out << ") returning " << DescribeType(type->return_type);
		return out.str();
	}
	return "<invalid>";
}

bool IsVoid(const Pa7TypePtr& type)
{
	Pa7TypePtr stripped = StripCV(type);
	return stripped && stripped->kind == PA7_TYPE_FUNDAMENTAL &&
		stripped->fundamental == FT_VOID;
}

bool IsFunction(const Pa7TypePtr& type)
{
	return StripCV(type) && StripCV(type)->kind == PA7_TYPE_FUNCTION;
}

bool IsConstQualified(const Pa7TypePtr& type)
{
	if (!type)
		return false;
	if (type->kind == PA7_TYPE_CV)
		return (type->cv & PA7_CV_CONST) != 0 ||
			(!type->children.empty() && IsConstQualified(type->children[0]));
	if (type->kind == PA7_TYPE_ARRAY && !type->children.empty())
		return IsConstQualified(type->children[0]);
	return false;
}

Pa7TypePtr AdjustParameter(const Pa7TypePtr& type)
{
	Pa7TypePtr adjusted = StripCV(type);
	if (!adjusted)
		return adjusted;
	if (adjusted->kind == PA7_TYPE_ARRAY && adjusted->children.size() == 1)
		return MakePointer(adjusted->children[0]);
	if (adjusted->kind == PA7_TYPE_FUNCTION)
		return MakePointer(adjusted);
	return adjusted;
}

bool SameType(const Pa7TypePtr& left, const Pa7TypePtr& right)
{
	return SameTypeImpl(left, right);
}

Pa7TypePtr MergeTypes(const Pa7TypePtr& first, const Pa7TypePtr& second)
{
	return MergeTypeImpl(first, second);
}

Pa7DeclAttributes::Pa7DeclAttributes()
	: storage(PA7_STORAGE_NONE), linkage(PA7_LINKAGE_UNSPECIFIED),
		linkage_explicit(false), is_const(false), is_constexpr(false),
		is_inline(false), defined(false),
		order(std::numeric_limits<std::size_t>::max())
{
}

Pa7Decl::Pa7Decl()
	: kind(PA7_DECL_VARIABLE), origin(PA7_DECL_OWNED), namespace_entity(0)
{
}

bool Pa7Decl::Matches(unsigned filter) const
{
	if (filter == PA7_FIND_ANY)
		return true;
	switch (kind)
	{
	case PA7_DECL_VARIABLE:
		return (filter & PA7_FIND_VARIABLE) != 0;
	case PA7_DECL_FUNCTION:
		return (filter & PA7_FIND_FUNCTION) != 0;
	case PA7_DECL_TYPEDEF:
		return (filter & PA7_FIND_TYPE) != 0;
	case PA7_DECL_NAMESPACE:
		return (filter & PA7_FIND_NAMESPACE) != 0;
	}
	return false;
}

Pa7Variable::Pa7Variable(const string& variable_name, const Pa7TypePtr& value,
	Pa7Namespace* namespace_owner)
	: name(variable_name), type(value), owner(namespace_owner),
		storage(PA7_STORAGE_NONE), linkage(PA7_LINKAGE_UNSPECIFIED),
		is_const(false), is_constexpr(false), defined(false),
		initially_defined(false),
		order(std::numeric_limits<std::size_t>::max()),
		initializer_expression(), declared_types()
{
}

Pa7Function::Pa7Function(const string& function_name, const Pa7TypePtr& value,
	Pa7Namespace* namespace_owner)
	: name(function_name), type(value), owner(namespace_owner),
		storage(PA7_STORAGE_NONE), linkage(PA7_LINKAGE_UNSPECIFIED),
		is_inline(false), defined(false),
		order(std::numeric_limits<std::size_t>::max())
{
}

Pa7Typedef::Pa7Typedef(const string& typedef_name, const Pa7TypePtr& value)
	: name(typedef_name), type(value)
{
}

Pa7Namespace::Pa7Namespace(const string& namespace_name,
	Pa7Namespace* namespace_parent, bool is_inline, bool is_unnamed)
	: name(namespace_name), parent(namespace_parent),
		inline_namespace(is_inline), unnamed(is_unnamed)
{
}

Pa7Decl* Pa7Namespace::FindDirect(const string& lookup_name, unsigned filter)
{
	map<string, Pa7Decl>::iterator it = declarations.find(lookup_name);
	if (it == declarations.end() || !it->second.Matches(filter))
		return 0;
	return &it->second;
}

const Pa7Decl* Pa7Namespace::FindDirect(const string& lookup_name,
	unsigned filter) const
{
	map<string, Pa7Decl>::const_iterator it = declarations.find(lookup_name);
	if (it == declarations.end() || !it->second.Matches(filter))
		return 0;
	return &it->second;
}

const Pa7Decl* Pa7Namespace::FindDirectOrInline(const string& lookup_name,
	unsigned filter) const
{
	const Pa7Decl* direct = FindDirect(lookup_name, filter);
	if (direct)
		return direct;
	for (size_t i = 0; i < namespaces.size(); ++i)
	{
		if (!namespaces[i]->inline_namespace)
			continue;
		const Pa7Decl* found = namespaces[i]->FindDirectOrInline(lookup_name,
			filter);
		if (found)
			return found;
	}
	return 0;
}

Pa7Namespace* Pa7Namespace::AddNamespace(const string& namespace_name,
	bool is_inline)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(namespace_name);
	if (existing != declarations.end())
	{
		if (existing->second.kind != PA7_DECL_NAMESPACE ||
			existing->second.origin != PA7_DECL_OWNED ||
			!existing->second.namespace_entity)
			throw runtime_error("namespace name conflicts with declaration");
		Pa7Namespace* result = existing->second.namespace_entity;
		// 7.3.1p8: only marking a previously non-inline namespace inline is
		// ill-formed; reopening an inline namespace without the keyword is
		// allowed and it stays inline (nsdecl-ref agrees).
		if (is_inline && !result->inline_namespace)
			throw runtime_error("non-inline namespace reopened as inline");
		return result;
	}

	shared_ptr<Pa7Namespace> result(new Pa7Namespace(namespace_name, this,
		is_inline, false));
	Pa7Decl decl;
	decl.kind = PA7_DECL_NAMESPACE;
	decl.origin = PA7_DECL_OWNED;
	decl.namespace_entity = result.get();
	declarations[namespace_name] = decl;
	namespaces.push_back(result);
	if (is_inline)
		AddUsingDirective(result.get());
	return result.get();
}

Pa7Namespace* Pa7Namespace::AddUnnamedNamespace(bool is_inline)
{
	if (unnamed_child)
	{
		if (is_inline && !unnamed_child->inline_namespace)
			throw runtime_error("non-inline namespace reopened as inline");
		AddUsingDirective(unnamed_child.get());
		return unnamed_child.get();
	}
	unnamed_child.reset(new Pa7Namespace(std::string(), this, is_inline,
		true));
	namespaces.push_back(unnamed_child);
	AddUsingDirective(unnamed_child.get());
	return unnamed_child.get();
}

void Pa7Namespace::AddNamespaceAlias(const string& alias_name,
	Pa7Namespace* target)
{
	if (!target)
		throw runtime_error("namespace alias target not found");
	map<string, Pa7Decl>::iterator existing = declarations.find(alias_name);
	if (existing != declarations.end())
	{
		if (existing->second.kind != PA7_DECL_NAMESPACE ||
			existing->second.origin != PA7_DECL_NAMESPACE_ALIAS ||
			existing->second.namespace_entity != target)
			throw runtime_error("namespace alias conflicts with declaration");
		return;
	}
	Pa7Decl decl;
	decl.kind = PA7_DECL_NAMESPACE;
	decl.origin = PA7_DECL_NAMESPACE_ALIAS;
	decl.namespace_entity = target;
	declarations[alias_name] = decl;
}

void Pa7Namespace::AddUsingDirective(Pa7Namespace* target)
{
	if (!target)
		return;
	for (size_t i = 0; i < using_directives.size(); ++i)
		if (using_directives[i] == target)
			return;
	using_directives.push_back(target);
}

void Pa7Namespace::AddUsingDeclaration(const string& declaration_name,
	const Pa7Decl& source)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(
		declaration_name);
	if (existing != declarations.end())
	{
		const Pa7Decl& prior = existing->second;
		bool same_entity = prior.kind == source.kind;
		switch (source.kind)
		{
		case PA7_DECL_VARIABLE:
			same_entity = same_entity && prior.variable == source.variable;
			break;
		case PA7_DECL_FUNCTION:
			same_entity = same_entity && prior.function == source.function;
			break;
		case PA7_DECL_TYPEDEF:
			same_entity = same_entity &&
				prior.typedef_entity == source.typedef_entity;
			break;
		case PA7_DECL_NAMESPACE:
			same_entity = same_entity &&
				prior.namespace_entity == source.namespace_entity;
			break;
		}
		if (prior.origin == PA7_DECL_USING && same_entity)
			return;
		throw runtime_error("using-declaration conflicts with declaration");
	}
	Pa7Decl imported = source;
	imported.origin = PA7_DECL_USING;
	declarations[declaration_name] = imported;
}

shared_ptr<Pa7Variable> Pa7Namespace::AddOrMergeVariable(
	const string& variable_name, const Pa7TypePtr& type,
	const Pa7DeclAttributes& attributes, bool strict)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(variable_name);
	if (existing != declarations.end())
	{
		if (existing->second.origin != PA7_DECL_OWNED ||
				existing->second.kind != PA7_DECL_VARIABLE ||
				!existing->second.variable)
			throw runtime_error("declaration kind conflict");
		if (!RedeclarationTypesAgree(existing->second.variable->type, type))
			throw runtime_error("variable type does not agree across redeclarations");
		CheckDeclarationAttributes(*existing->second.variable, attributes, strict);
		existing->second.variable->type = MergeTypes(
			existing->second.variable->type, type);
		ApplyVariableAttributes(*existing->second.variable, attributes);
		if (strict)
			existing->second.variable->declared_types.push_back(type);
		return existing->second.variable;
	}
	shared_ptr<Pa7Variable> result(new Pa7Variable(variable_name, type, this));
	result->storage = attributes.storage;
	result->linkage = attributes.linkage;
	result->is_const = attributes.is_const;
	result->is_constexpr = attributes.is_constexpr;
	result->defined = attributes.defined;
	result->initially_defined = attributes.defined;
	result->order = attributes.order;
	if (strict)
		result->declared_types.push_back(type);
	Pa7Decl decl;
	decl.kind = PA7_DECL_VARIABLE;
	decl.variable = result;
	declarations[variable_name] = decl;
	variables.push_back(result);
	return result;
}

shared_ptr<Pa7Function> Pa7Namespace::AddOrMergeFunction(
	const string& function_name, const Pa7TypePtr& type,
	const Pa7DeclAttributes& attributes, bool strict)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(function_name);
	if (existing != declarations.end())
	{
		if (existing->second.origin != PA7_DECL_OWNED ||
				existing->second.kind != PA7_DECL_FUNCTION ||
				!existing->second.function)
			throw runtime_error("declaration kind conflict");
		vector<shared_ptr<Pa7Function> >& overloads =
			existing->second.function_overloads;
		if (overloads.empty())
			overloads.push_back(existing->second.function);
		for (size_t i = 0; i < overloads.size(); ++i)
		{
			if (!SameType(overloads[i]->type, type))
				continue;
			CheckDeclarationAttributes(*overloads[i], attributes, strict);
			overloads[i]->type = MergeTypes(overloads[i]->type, type);
			ApplyFunctionAttributes(*overloads[i], attributes);
			return overloads[i];
		}
		if (strict)
			for (size_t i = 0; i < overloads.size(); ++i)
				if (SameSignature(overloads[i]->type, type))
					throw runtime_error(
						"function redeclaration differs only in return type");
		shared_ptr<Pa7Function> result(new Pa7Function(function_name, type,
			this));
		result->storage = attributes.storage;
		result->linkage = attributes.linkage;
		result->is_inline = attributes.is_inline;
		result->defined = attributes.defined;
		result->order = attributes.order;
		overloads.push_back(result);
		functions.push_back(result);
		return result;
	}
	shared_ptr<Pa7Function> result(new Pa7Function(function_name, type, this));
	result->storage = attributes.storage;
	result->linkage = attributes.linkage;
	result->is_inline = attributes.is_inline;
	result->defined = attributes.defined;
	result->order = attributes.order;
	Pa7Decl decl;
	decl.kind = PA7_DECL_FUNCTION;
	decl.function = result;
	decl.function_overloads.push_back(result);
	declarations[function_name] = decl;
	functions.push_back(result);
	return result;
}

shared_ptr<Pa7Typedef> Pa7Namespace::AddTypedef(const string& typedef_name,
	const Pa7TypePtr& type)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(typedef_name);
	if (existing != declarations.end())
	{
		if (existing->second.origin != PA7_DECL_OWNED ||
			existing->second.kind != PA7_DECL_TYPEDEF ||
			!existing->second.typedef_entity)
			throw runtime_error("declaration kind conflict");
		return existing->second.typedef_entity;
	}
	shared_ptr<Pa7Typedef> result(new Pa7Typedef(typedef_name, type));
	Pa7Decl decl;
	decl.kind = PA7_DECL_TYPEDEF;
	decl.typedef_entity = result;
	declarations[typedef_name] = decl;
	return result;
}

void PrintTranslationUnit(ostream& out, const Pa7Namespace& global)
{
	PrintNamespace(out, global, true);
}
