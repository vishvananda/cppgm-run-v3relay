#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "nsdecl_model.h"
#include "recog_token.h"

class Pa7Parser
{
public:
	Pa7Parser(const std::vector<Pa6Token>& tokens, Pa7Namespace* global);

	bool ParseTranslationUnit();

private:
	enum DeclaratorMode
	{
		DECL_NAMED,
		DECL_ABSTRACT,
		DECL_EITHER
	};

	struct NamePath
	{
		bool absolute;
		std::vector<std::string> parts;

		NamePath() : absolute(false) {}
	};

	struct Declarator
	{
		Pa7TypePtr type;
		bool has_name;
		NamePath path;

		Declarator() : has_name(false) {}
	};

	struct DeclSpec
	{
		Pa7TypePtr type;
		bool is_typedef;

		DeclSpec() : is_typedef(false) {}
	};

	struct Parameters
	{
		std::vector<Pa7TypePtr> types;
		bool varargs;

		Parameters() : varargs(false) {}
	};

	struct FundamentalSpec
	{
		bool have_char;
		bool have_char16;
		bool have_char32;
		bool have_wchar;
		bool have_bool;
		bool have_int;
		bool have_float;
		bool have_double;
		bool have_void;
		bool have_signed;
		bool have_unsigned;
		unsigned short_count;
		unsigned long_count;

		FundamentalSpec();
		bool HasType() const;
		Pa7TypePtr Finish() const;
	};

	void ParseDeclaration();
	void ParseNamespaceDefinition();
	void ParseNamespaceAliasDefinition();
	void ParseUsingDeclaration();
	void ParseSimpleDeclaration();

	DeclSpec ParseDeclSpecifierSeq();
	Pa7TypePtr ParseTypeId();
	Declarator ParseDeclarator(const Pa7TypePtr& base, DeclaratorMode mode);
	Declarator ParsePtrDeclarator(const Pa7TypePtr& base,
		DeclaratorMode mode);
	Parameters ParseParametersAndQualifiers();
	Declarator ParseParameterDeclaration();

	Pa7TypePtr InsertFunctionSuffix(const Pa7TypePtr& type,
		const Parameters& parameters);
	Pa7TypePtr InsertArraySuffix(const Pa7TypePtr& type, bool has_bound,
		unsigned long long bound);
	Pa7TypePtr ParseArraySuffix(const Pa7TypePtr& type, bool grouped);

	NamePath ParseNamePath();
	Pa7TypePtr LookupTypedef(const NamePath& path) const;
	Pa7Namespace* ResolveNamespace(const NamePath& path) const;
	Pa7Decl* LookupUnqualified(const std::string& name,
		unsigned filter) const;
	Pa7Decl* LookupInUsing(const Pa7Namespace* scope,
		const std::string& name, unsigned filter,
		std::vector<const Pa7Namespace*>& visited) const;
	Pa7Namespace* ResolveRelativeNamespace(const Pa7Namespace* scope,
		const std::vector<std::string>& parts, bool absolute) const;

	bool IsParameterStart(std::size_t at) const;
	bool IsDeclSpecifierStart(std::size_t at) const;
	bool IsDeclaratorStart(std::size_t at) const;
	bool TryRedundantParentheses(const Pa7TypePtr& base,
		DeclaratorMode mode, Declarator& result);

	const Pa6Token& Token(std::size_t at) const;
	bool IsSimple(ETokenType type, std::size_t at = static_cast<std::size_t>(-1)) const;
	bool IsKind(Pa6TokenKind kind, std::size_t at = static_cast<std::size_t>(-1)) const;
	bool ConsumeSimple(ETokenType type);
	bool ConsumeKind(Pa6TokenKind kind);
	bool ConsumeIdentifier(std::string* spelling = 0);
	bool ConsumeLiteral(Pa6Token* literal = 0);
	void ExpectSimple(ETokenType type);
	void ExpectIdentifier(std::string* spelling = 0);
	void ExpectEndOfDeclaration();

	const std::vector<Pa6Token>& tokens_;
	std::size_t pos_;
	Pa7Namespace* global_;
	std::vector<Pa7Namespace*> scopes_;
};
