#include "sema/qualified_name.h"

#include <stdexcept>

QualifiedName::QualifiedName()
    : global(false), template_id(false), template_first(0), template_last(0)
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
  {
    throw std::runtime_error("empty qualified name");
  }
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
                                std::size_t first, std::size_t last,
                                bool allow_template_id)
{
  QualifiedName result;
  if (last > tokens.size())
    last = tokens.size();
  bool expect_identifier = true;
	for (std::size_t i = first; i < last; ++i)
	{
		const Pa6Token& token = tokens[i];
		if (token.IsSimple(KW_OPERATOR) && expect_identifier)
		{
			// Operator-function-ids are the one qualified-name component
			// whose spelling is carried by punctuation/keyword tokens rather
			// than an identifier token.  The parser has already delimited the
			// component, so consume that complete span here and keep one
			// canonical lookup spelling (operator+, operator[], operator()).
			std::string spelling = "operator";
			for (std::size_t part = i + 1; part < last; ++part)
				spelling += tokens[part].spelling;
			result.components.push_back(spelling);
			expect_identifier = false;
			break;
		}
		if (token.kind == PA6_IDENTIFIER_TOKEN && expect_identifier)
    {
      result.components.push_back(token.spelling);
      expect_identifier = false;
      if (i + 1 < last && tokens[i + 1].IsSimple(OP_LT))
      {
        if (!allow_template_id)
          throw std::runtime_error("template-id is not supported here");
        // Each half of a split `>>` closes one argument list (14.2p3).
        std::size_t depth = 1;
        std::size_t close = i + 2;
        for (; close < last && depth != 0; ++close)
        {
          if (tokens[close].IsSimple(OP_LT))
            ++depth;
          else if (tokens[close].IsSimple(OP_GT) || tokens[close].IsRshiftPart())
            --depth;
        }
        if (depth != 0)
          throw std::runtime_error("template-id has an incomplete argument list");
        if (close != last)
          throw std::runtime_error("template-id is not supported as a qualifier");
        result.template_id = true;
        result.template_first = i + 2;
        result.template_last = close - 1;
        break;
      }
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
      if (i + 1 < last && tokens[i + 1].IsSimple(KW_TEMPLATE))
        ++i;
      continue;
    }
    throw std::runtime_error("unsupported name form");
  }
  if (expect_identifier && !(result.components.empty() && !result.global))
    throw std::runtime_error("qualified name ends with ::");
  return result;
}
