#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "parser/recog_token.h"

// A name read once from its token span: an optional leading `::`, the bare
// identifier components, and - when the reader allows one - the
// template-argument list of the last component as the half-open token range
// between its `<` and `>`.  Nothing about a template-id is encoded in the
// component spellings, so no consumer has to find `<` again.
struct QualifiedName
{
  bool global;
  std::vector<std::string> components;
  bool template_id;
  std::size_t template_first;
  std::size_t template_last;

  QualifiedName();

  bool Empty() const;
  bool Qualified() const; // has a leading `::` or more than one component
  const std::string& Last() const;
  QualifiedName Prefix() const; // everything before the last component
  std::string Joined() const;   // presentation spelling `a::b`
};

// Reads [first, last).  A template-id is accepted only as the last component
// and only for expression names (`allow_template_id`); declarations, type
// names and every other unsupported name form are rejected here.
QualifiedName ReadQualifiedName(const std::vector<Pa6Token>& tokens,
                                std::size_t first, std::size_t last,
                                bool allow_template_id = false);
