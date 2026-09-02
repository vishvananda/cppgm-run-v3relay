#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "parser/recog_token.h"

// A name read once from its token span: an optional leading `::` and the
// identifier components.  Template-id arguments are retained in the last
// component's presentation spelling (for example `hello<stream>`); semantic
// lookup strips that spelling only when it performs template instantiation.
struct QualifiedName
{
  bool global;
  std::vector<std::string> components;

  QualifiedName();

  bool Empty() const;
  bool Qualified() const; // has a leading `::` or more than one component
  const std::string& Last() const;
  QualifiedName Prefix() const; // everything before the last component
  std::string Joined() const;   // presentation spelling `a::b`
};

QualifiedName ReadQualifiedName(const std::vector<Pa6Token>& tokens,
                                std::size_t first, std::size_t last);
