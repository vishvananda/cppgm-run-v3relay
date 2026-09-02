#include "sema/qualified_name.h"

#include <stdexcept>

QualifiedName::QualifiedName()
    : global(false)
{
}

bool QualifiedName::Empty() const
{
  return components.empty();
}

bool QualifiedName::Qualified() const
{
  return global || components.size() > 1;
}

const std::string& QualifiedName::Last() const
{
  if (components.empty())
    throw std::runtime_error("empty qualified name");
  return components.back();
}

QualifiedName QualifiedName::Prefix() const
{
  QualifiedName prefix;
  prefix.global = global;
  if (!components.empty())
    prefix.components.assign(components.begin(), components.end() - 1);
  return prefix;
}

std::string QualifiedName::Joined() const
{
  std::string result = global ? "::" : "";
  for (std::size_t i = 0; i < components.size(); ++i)
  {
    if (i != 0)
      result += "::";
    result += components[i];
  }
  return result;
}

QualifiedName ReadQualifiedName(const std::vector<Pa6Token>& tokens,
                                std::size_t first, std::size_t last)
{
  QualifiedName result;
  bool expect_identifier = true;
  for (std::size_t i = first; i < last && i < tokens.size(); ++i)
  {
    const Pa6Token& token = tokens[i];
    if (token.kind == PA6_IDENTIFIER_TOKEN && expect_identifier)
    {
      result.components.push_back(token.spelling);
      expect_identifier = false;
      continue;
    }
    if (token.IsSimple(OP_COLON2) && expect_identifier && i == first)
    {
      result.global = true;
      continue;
    }
    if (token.IsSimple(OP_COLON2) && !expect_identifier)
    {
      expect_identifier = true;
      continue;
    }
    throw std::runtime_error("unsupported name form in PA11");
  }
  if (expect_identifier && !(result.components.empty() && !result.global))
    throw std::runtime_error("qualified name ends with ::");
  return result;
}
