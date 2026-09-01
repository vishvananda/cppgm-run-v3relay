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
			(!left->has_bound || left->bound == right->bound) &&
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
		const unsigned long long bound = first->has_bound ? first->bound :
			second->bound;
		Pa7TypePtr first_element = first->children.empty() ? Pa7TypePtr() :
			first->children[0];
		Pa7TypePtr second_element = second->children.empty() ? Pa7TypePtr() :
			second->children[0];
		return MakeArray(has_bound, bound,
			MergeTypeImpl(first_element, second_element));
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

Pa7Type::Pa7Type()
	: kind(PA7_TYPE_FUNDAMENTAL), fundamental(FT_INT), cv(PA7_CV_NONE),
		has_bound(false), bound(0), varargs(false), return_type()
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
			ApplyCV(cv, type->children[0]));
	if (type->kind == PA7_TYPE_CV && type->children.size() == 1)
		return ApplyCV(type->cv | cv, type->children[0]);
	return MakeCV(cv, type);
}

Pa7TypePtr MakePointer(const Pa7TypePtr& inner)
{
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_POINTER;
	result->children.push_back(inner);
	return result;
}

Pa7TypePtr MakeReference(bool is_rvalue, const Pa7TypePtr& inner)
{
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
	const Pa7TypePtr& element)
{
	Pa7TypePtr result(new Pa7Type);
	result->kind = PA7_TYPE_ARRAY;
	result->has_bound = has_bound;
	result->bound = bound;
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

Pa7Decl::Pa7Decl()
	: kind(PA7_DECL_VARIABLE)
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

Pa7Variable::Pa7Variable(const string& variable_name, const Pa7TypePtr& value)
	: name(variable_name), type(value)
{
}

Pa7Function::Pa7Function(const string& function_name, const Pa7TypePtr& value)
	: name(function_name), type(value)
{
}

Pa7Typedef::Pa7Typedef(const string& typedef_name, const Pa7TypePtr& value)
	: name(typedef_name), type(value)
{
}

Pa7Namespace::Pa7Namespace(const string& namespace_name,
	Pa7Namespace* namespace_parent, bool is_inline, bool is_unnamed)
	: name(namespace_name), parent(namespace_parent),
		inline_namespace(is_inline), unnamed(is_unnamed), alias_target(0)
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

Pa7Namespace* Pa7Namespace::FindNamespaceDirect(const string& lookup_name)
	const
{
	const Pa7Decl* decl = FindDirect(lookup_name, PA7_FIND_NAMESPACE);
	return decl && decl->namespace_entity ? decl->namespace_entity.get() : 0;
}

Pa7Namespace* Pa7Namespace::AddNamespace(const string& namespace_name,
	bool is_inline)
{
	Pa7Decl* existing = FindDirect(namespace_name, PA7_FIND_NAMESPACE);
	if (existing && existing->namespace_entity)
	{
		Pa7Namespace* result = existing->namespace_entity.get();
		if (result->inline_namespace != is_inline)
			throw runtime_error("namespace inline status changed");
		return result;
	}
	if (declarations.find(namespace_name) != declarations.end())
		throw runtime_error("namespace name conflicts with declaration");

	shared_ptr<Pa7Namespace> result(new Pa7Namespace(namespace_name, this,
		is_inline, false));
	Pa7Decl decl;
	decl.kind = PA7_DECL_NAMESPACE;
	decl.namespace_entity = result;
	declarations[namespace_name] = decl;
	namespaces.push_back(result);
	return result.get();
}

Pa7Namespace* Pa7Namespace::AddUnnamedNamespace(bool is_inline)
{
	if (unnamed_child)
	{
		if (unnamed_child->inline_namespace != is_inline)
			throw runtime_error("namespace inline status changed");
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
			existing->second.namespace_entity.get() != target)
			throw runtime_error("namespace alias conflicts with declaration");
		return;
	}
	Pa7Decl decl;
	decl.kind = PA7_DECL_NAMESPACE;
	decl.namespace_entity.reset(target, [](Pa7Namespace*) {});
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

shared_ptr<Pa7Variable> Pa7Namespace::AddOrMergeVariable(
	const string& variable_name, const Pa7TypePtr& type)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(variable_name);
	if (existing != declarations.end())
	{
		if (existing->second.kind != PA7_DECL_VARIABLE ||
			!existing->second.variable)
			throw runtime_error("declaration kind conflict");
		existing->second.variable->type = MergeTypes(
			existing->second.variable->type, type);
		return existing->second.variable;
	}
	shared_ptr<Pa7Variable> result(new Pa7Variable(variable_name, type));
	Pa7Decl decl;
	decl.kind = PA7_DECL_VARIABLE;
	decl.variable = result;
	declarations[variable_name] = decl;
	variables.push_back(result);
	return result;
}

shared_ptr<Pa7Function> Pa7Namespace::AddOrMergeFunction(
	const string& function_name, const Pa7TypePtr& type)
{
	map<string, Pa7Decl>::iterator existing = declarations.find(function_name);
	if (existing != declarations.end())
	{
		if (existing->second.kind != PA7_DECL_FUNCTION ||
			!existing->second.function)
			throw runtime_error("declaration kind conflict");
		return existing->second.function;
	}
	shared_ptr<Pa7Function> result(new Pa7Function(function_name, type));
	Pa7Decl decl;
	decl.kind = PA7_DECL_FUNCTION;
	decl.function = result;
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
		if (existing->second.kind != PA7_DECL_TYPEDEF ||
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
