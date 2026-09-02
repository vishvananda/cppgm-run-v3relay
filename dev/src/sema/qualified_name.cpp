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
  std::size_t i = first;
  bool expect_identifier = true;
  while (i < last && i < tokens.size())
  {
    const Pa6Token& token = tokens[i];
    if (token.kind == PA6_IDENTIFIER_TOKEN && expect_identifier)
    {
      std::string component = token.spelling;
      ++i;
      if (i < last && i < tokens.size() && tokens[i].IsSimple(OP_LT))
      {
        std::size_t depth = 0;
        while (i < last && i < tokens.size())
        {
          const Pa6Token& argument = tokens[i];
          if (argument.IsSimple(OP_LT))
          {
            ++depth;
            component += '<';
          }
          else if (argument.IsSimple(OP_GT) || argument.IsRshiftPart())
          {
            if (depth == 0)
              throw std::runtime_error("template name has unmatched >");
            --depth;
            component += '>';
          }
          else
            component += argument.spelling;
          ++i;
          if (depth == 0)
            break;
        }
        if (depth != 0)
          throw std::runtime_error("template name has an incomplete argument list");
      }
      result.components.push_back(component);
      expect_identifier = false;
      continue;
    }
    if (token.IsSimple(OP_COLON2) && expect_identifier && i == first)
    {
      result.global = true;
      ++i;
      continue;
    }
    if (token.IsSimple(OP_COLON2) && !expect_identifier)
    {
      expect_identifier = true;
      ++i;
      if (i < last && i < tokens.size() && tokens[i].IsSimple(KW_TEMPLATE))
        ++i;
      continue;
    }
    throw std::runtime_error("unsupported name form in PA11");
  }
  if (expect_identifier && !(result.components.empty() && !result.global))
    throw std::runtime_error("qualified name ends with ::");
  return result;
}
