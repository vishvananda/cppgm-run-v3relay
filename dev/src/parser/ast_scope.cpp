#include "ast_scope.h"

SyntaxScopes::SyntaxScopes()
	: scopes_(1), scope_marks_(1, 0)
{
}

void SyntaxScopes::Push()
{
	scopes_.push_back(Scope());
	scope_marks_.push_back(undo_.size());
}

void SyntaxScopes::Pop()
{
	if (scopes_.size() <= 1)
		return;
	Rollback(scope_marks_.back());
	scope_marks_.pop_back();
	scopes_.pop_back();
}

std::size_t SyntaxScopes::Mark() const
{
	return undo_.size();
}

bool SyntaxScopes::Rollback(std::size_t mark)
{
	bool changed = false;
	while (undo_.size() > mark)
	{
		const Undo undo = undo_.back();
		undo_.pop_back();
		if (undo.scope_depth >= scopes_.size())
			continue;
		Scope& scope = scopes_[undo.scope_depth];
		if (undo.had_previous)
			scope[undo.name] = undo.previous;
		else
			scope.erase(undo.name);
		changed = true;
	}
	return changed;
}

void SyntaxScopes::Bind(const std::string& name, BindKind kind)
{
	if (name.empty())
		return;
	Scope& scope = scopes_.back();
	const Scope::const_iterator found = scope.find(name);
	Undo undo;
	undo.scope_depth = scopes_.size() - 1;
	undo.name = name;
	undo.had_previous = found != scope.end();
	undo.previous = undo.had_previous ? found->second : BIND_VALUE;
	undo_.push_back(undo);
	scope[name] = kind;
}

const BindKind* SyntaxScopes::Lookup(const std::string& name) const
{
	for (std::size_t i = scopes_.size(); i != 0; --i)
	{
		const Scope::const_iterator found = scopes_[i - 1].find(name);
		if (found != scopes_[i - 1].end())
			return &found->second;
	}
	return 0;
}

const BindKind* SyntaxScopes::LookupScopePrefix(const std::string& name) const
{
	for (std::size_t i = scopes_.size(); i != 0; --i)
	{
		const Scope::const_iterator found = scopes_[i - 1].find(name);
		if (found != scopes_[i - 1].end() && found->second != BIND_VALUE)
			return &found->second;
	}
	return 0;
}
