#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "parser/recog_token.h"

// A name read once from its token span: an optional leading `::` and the
// identifier components.  PA11 supports no template-ids, operator names,
// destructor names, or decltype qualifiers, so any other token in the span is
// rejected here rather than being folded into a component spelling.
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
