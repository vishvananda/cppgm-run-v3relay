#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

enum BindKind
{
	BIND_TYPE,
	BIND_TEMPLATE,
	BIND_NAMESPACE,
	BIND_VALUE
};

// Syntactic name classification for the PA10 parser: a stack of scopes with
// an undo log so speculative declarations can be rolled back transactionally.
class SyntaxScopes
{
public:
	SyntaxScopes();

	void Push();
	void Pop();
	std::size_t Mark() const;
	// Undoes every binding made after mark; returns whether anything changed.
	bool Rollback(std::size_t mark);
	void Bind(const std::string& name, BindKind kind);
	const BindKind* Lookup(const std::string& name) const;
	// Lookup for a name before `::` (3.4.3p1): value bindings are skipped.
	const BindKind* LookupScopePrefix(const std::string& name) const;

private:
	typedef std::unordered_map<std::string, BindKind> Scope;

	struct Undo
	{
		std::size_t scope_depth;
		std::string name;
		bool had_previous;
		BindKind previous;
	};

	std::vector<Scope> scopes_;
	std::vector<Undo> undo_;
	std::vector<std::size_t> scope_marks_;
};
