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

class SyntaxScopes
{
public:
	SyntaxScopes();

	void Push();
	void Pop();
	std::size_t Mark() const;
	bool Rollback(std::size_t mark);
	void Bind(const std::string& name, BindKind kind);
	const BindKind* Lookup(const std::string& name) const;
	const BindKind* LookupScopePrefix(const std::string& name) const;

private:
	struct Undo
	{
		std::size_t scope_depth;
		std::string name;
		BindKind kind;
		bool had_previous;
		BindKind previous;
	};

	std::vector<std::unordered_map<std::string, BindKind> > scopes_;
	std::vector<Undo> undo_;
	std::vector<std::size_t> scope_marks_;
};
