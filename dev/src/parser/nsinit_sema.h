#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nsdecl_model.h"
#include "recog_token.h"

struct Pa8Expr
{
	Pa8ExprKind kind;
	Pa6Token literal;
	bool absolute;
	std::vector<std::string> path;
	Pa7Namespace* lookup_scope;
	std::size_t token_index;
	// Count of declarations committed in this translation unit when the
	// expression was parsed; only earlier declarations are visible to it.
	std::size_t decl_epoch;

	Pa8Expr();
};

inline Pa8Expr::Pa8Expr()
	: kind(PA8_EXPR_LITERAL), literal(PA6_EOF_TOKEN, ""), absolute(false),
		lookup_scope(0), token_index(0), decl_epoch(0)
{
}

struct Pa8Relocation
{
	std::size_t offset;
	std::string symbol;
	long long addend;

	Pa8Relocation(std::size_t offset = 0,
		const std::string& symbol = std::string(), long long addend = 0)
		: offset(offset), symbol(symbol), addend(addend) {}
};

struct Pa8Value
{
	std::vector<unsigned char> bytes;
	std::vector<Pa8Relocation> relocs;
	// An lvalue carries its address separately from the relocations in its
	// loaded value.  This matters for a constant lvalue: it can participate in
	// both lvalue-to-rvalue evaluation and address/reference binding.
	std::vector<Pa8Relocation> address_relocs;
	Pa7TypePtr type;
	bool is_constant;
	bool is_lvalue;
	bool is_string_literal;
	std::string string_symbol;

	Pa8Value();
};

struct Pa8Temporary
{
	std::string symbol;
	Pa7TypePtr type;
	Pa8Value value;

	Pa8Temporary();
};

struct Pa8StringLiteral
{
	std::string symbol;
	std::vector<unsigned char> bytes;
	std::size_t alignment;

	Pa8StringLiteral();
};

struct Pa8ProgramEntity
{
	bool is_function;
	std::string key;
	Pa7Variable* variable;
	Pa7Function* function;
	Pa7TypePtr type;
	std::vector<Pa7Variable*> variables;
	std::vector<Pa7Function*> functions;
	std::size_t first_unit;
	std::size_t first_order;
	bool defined;
	std::size_t offset;
	Pa8Value value;

	Pa8ProgramEntity();
};

// The PA8 semantic pass owns the program-wide identity table.  The parser
// records syntax and declaration attributes in each translation-unit model;
// this pass resolves those declarations, evaluates constant initializers, and
// leaves relocations symbolic for the image writer.
class Pa8ProgramSema
{
public:
	Pa8ProgramSema(
		const std::vector<std::shared_ptr<Pa7Namespace> >& globals);

	void Analyze();
	const std::vector<Pa8ProgramEntity>& Entities() const;
	const std::vector<Pa8Temporary>& Temporaries() const;
	const std::vector<Pa8StringLiteral>& Strings() const;

	std::string SymbolFor(const Pa7Variable* variable) const;
	std::string SymbolFor(const Pa7Function* function) const;

	std::size_t TypeSize(const Pa7TypePtr& type);
	std::size_t TypeAlignment(const Pa7TypePtr& type);

private:
	struct EvalState
	{
		bool active;
		bool complete;
		Pa8Value value;

		EvalState() : active(false), complete(false), value() {}
	};

	struct ResolvedDecl
	{
		Pa7Decl* declaration;
		Pa7Namespace* scope;

		ResolvedDecl() : declaration(0), scope(0) {}
	};

	struct Numeric
	{
		bool valid;
		bool floating;
		long double floating_value;
		long long signed_value;
		unsigned long long unsigned_value;

		Numeric()
			: valid(false), floating(false), floating_value(0), signed_value(0),
				unsigned_value(0) {}
	};

	void CollectNamespace(Pa7Namespace* scope, std::size_t unit);
	void AddVariable(Pa7Variable* variable, std::size_t unit);
	void AddFunction(Pa7Function* function, std::size_t unit);
	std::string NamespacePath(const Pa7Namespace* scope) const;
	std::string TypeKey(const Pa7TypePtr& type) const;
	std::string MakeVariableKey(const Pa7Variable* variable,
		std::size_t unit) const;
	std::string MakeFunctionKey(const Pa7Function* function,
		std::size_t unit) const;
	Pa8ProgramEntity* FindOrCreateEntity(const std::string& key,
		bool is_function, std::size_t unit, std::size_t order);

	ResolvedDecl ResolveExpression(const Pa8Expr& expression) const;
	Pa7Decl* LookupInNamespace(const Pa7Namespace* scope,
		const std::string& name, unsigned filter) const;
	Pa7Decl* LookupUsing(const Pa7Namespace* scope, const std::string& name,
		unsigned filter, std::map<const Pa7Namespace*, bool>& visited) const;
	Pa7Namespace* ResolveNamespace(const Pa8Expr& expression,
		std::size_t last_part) const;

	Pa8Value EvaluateExpression(const Pa8ExprPtr& expression,
		bool require_constant = false);
	Pa8Value EvaluateVariable(Pa7Variable* variable);
	Pa8Value EvaluateReferencedValue(const Pa8Value& reference,
		const Pa7TypePtr& type);
	Pa8Value EvaluateLiteral(const Pa8Expr& expression);
	Pa8Value Convert(const Pa8Value& source, const Pa7TypePtr& target,
		bool require_constant);
	Pa8Value LoadLvalue(const Pa8Value& source, bool require_constant);
	bool PointerConvertible(const Pa7TypePtr& source,
		const Pa7TypePtr& target) const;
	void CheckDeclaredTypeAgreement();
	Numeric DecodeNumeric(const Pa8Value& value) const;
	Pa8Value EncodeNumeric(const Numeric& value, const Pa7TypePtr& target);
	Pa8Value NullValue(const Pa7TypePtr& type) const;
	bool IsConstType(const Pa7TypePtr& type) const;
	bool IsIntegralType(const Pa7TypePtr& type) const;
	bool IsFloatingType(const Pa7TypePtr& type) const;
	Pa7TypePtr StripCV(const Pa7TypePtr& type) const;
	Pa7TypePtr Pointee(const Pa7TypePtr& type) const;
	std::size_t FundamentalSize(EFundamentalType type) const;
	std::size_t FundamentalAlignment(EFundamentalType type) const;
	unsigned long long ArrayBound(const Pa7TypePtr& type);
	void ResolveArrayBounds(const Pa7TypePtr& type);
	std::string RegisterTemporary(const Pa7TypePtr& type,
		const Pa8Value& source);
	std::string RegisterStringLiteral(const Pa8Expr& expression);
	std::vector<unsigned char> DecodeString(const std::string& spelling) const;
	bool IsZero(const Pa8Value& value) const;
	bool IsPointerLike(const Pa7TypePtr& type) const;

	const std::vector<std::shared_ptr<Pa7Namespace> >& globals_;
	std::vector<Pa8ProgramEntity> entities_;
	std::vector<Pa8Temporary> temporaries_;
	std::vector<Pa8StringLiteral> strings_;
	std::map<std::string, std::size_t> entity_by_key_;
	// All linked entities sharing one variable key, in creation order;
	// duplicate plain definitions produce multiple entities (probe p71).
	std::map<std::string, std::vector<std::size_t> > linked_variables_;
	std::map<const Pa7Variable*, std::size_t> variable_entities_;
	std::map<const Pa7Function*, std::size_t> function_entities_;
	std::map<const Pa7Variable*, EvalState> variable_states_;
	std::map<std::string, std::size_t> temporary_by_symbol_;
	std::map<std::string, std::size_t> string_by_symbol_;
	std::map<const Pa8Expr*, std::string> string_symbols_;
	std::size_t next_temporary_id_;
	std::size_t next_string_id_;
	// static_assert operands are evaluated with collection off: their string
	// literals convert to bool but are not emitted to BLOCK3.
	bool collect_strings_;
};
