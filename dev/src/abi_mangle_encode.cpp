#include "abi_mangle_encoder.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace abi_mangle {
namespace {

std::string normalized_name(const std::string & name)
{
  return name.compare(0, 2, "::") == 0 ? name.substr(2) : name;
}

std::vector<std::string> name_components(const std::string & name)
{
  const std::string normalized = normalized_name(name);
  std::vector<std::string> components;
  std::size_t start = 0;
  while(start <= normalized.size()) {
    const std::size_t separator = normalized.find("::", start);
    if(separator == std::string::npos) {
      if(start == normalized.size()) {
        throw std::logic_error("ABI qualified name has an empty component");
      }
      components.push_back(normalized.substr(start));
      break;
    }
    if(separator == start) {
      throw std::logic_error("ABI qualified name has an empty component");
    }
    components.push_back(normalized.substr(start, separator - start));
    start = separator + 2;
  }
  if(components.empty()) {
    throw std::logic_error("ABI qualified name is empty");
  }
  return components;
}

std::string joined_components(const std::vector<std::string> & components,
                              std::size_t last)
{
  std::string result;
  for(std::size_t i = 0; i <= last; ++i) {
    if(i != 0) result += "::";
    result += components[i];
  }
  return result;
}

std::string source_name(const std::string & name)
{
  return std::to_string(name.size()) + name;
}

bool is_dash(const std::string & word)
{
  return word.empty() || word == "-";
}

std::string signed_number(long long value)
{
  if(value >= 0) {
    return std::to_string(static_cast<unsigned long long>(value));
  }
  const unsigned long long magnitude = 0ULL -
    static_cast<unsigned long long>(value);
  return "n" + std::to_string(magnitude);
}

std::string operator_code(const std::string & name)
{
  if(name == "new") return "nw";
  if(name == "new-array") return "na";
  if(name == "delete") return "dl";
  if(name == "delete-array") return "da";
  if(name == "plus") return "pl";
  if(name == "minus") return "mi";
  if(name == "multiply") return "ml";
  if(name == "divide") return "dv";
  if(name == "modulo") return "rm";
  if(name == "plus-assign") return "pL";
  if(name == "minus-assign") return "mI";
  if(name == "multiply-assign") return "mL";
  if(name == "divide-assign") return "dV";
  if(name == "modulo-assign") return "rM";
  if(name == "equal") return "eq";
  if(name == "not-equal") return "ne";
  if(name == "less") return "lt";
  if(name == "greater") return "gt";
  if(name == "less-equal") return "le";
  if(name == "greater-equal") return "ge";
  if(name == "subscript") return "ix";
  if(name == "call") return "cl";
  if(name == "arrow") return "pt";
  if(name == "arrow-star") return "pm";
  if(name == "dereference") return "de";
  if(name == "address-of") return "ad";
  if(name == "increment") return "pp";
  if(name == "decrement") return "mm";
  if(name == "unary-plus") return "ps";
  if(name == "unary-minus") return "ng";
  if(name == "bit-and") return "an";
  if(name == "bit-or") return "or";
  if(name == "bit-xor") return "eo";
  if(name == "complement") return "co";
  if(name == "logical-not") return "nt";
  if(name == "logical-and") return "aa";
  if(name == "logical-or") return "oo";
  if(name == "comma") return "cm";
  if(name == "assign") return "aS";
  if(name == "spaceship") return "ss";
  if(name == "co-await") return "aw";
  throw std::logic_error("unknown ABI operator terminal '" + name + "'");
}

void append_qualifier_codes(const std::vector<AbiFunctionQualifier> & qualifiers,
                            std::string * result)
{
  bool has_volatile = false;
  bool has_const = false;
  bool has_lvalue_ref = false;
  bool has_rvalue_ref = false;
  for(std::size_t i = 0; i < qualifiers.size(); ++i) {
    has_volatile = has_volatile ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_VOLATILE;
    has_const = has_const || qualifiers[i] == ABI_FUNCTION_QUALIFIER_CONST;
    has_lvalue_ref = has_lvalue_ref ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE;
    has_rvalue_ref = has_rvalue_ref ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE;
  }
  if(has_volatile) *result += "V";
  if(has_const) *result += "K";
  if(has_lvalue_ref) *result += "R";
  if(has_rvalue_ref) *result += "O";
}

std::string local_discriminator_suffix(const std::string & discriminator)
{
  if(discriminator.empty() || discriminator == "-") {
    return std::string();
  }
  unsigned long long value = 0;
  std::istringstream input(discriminator);
  if(!(input >> value) || !input.eof()) {
    throw std::logic_error("invalid ABI local discriminator '" +
      discriminator + "'");
  }
  return value == 0 ? std::string() : "_" + std::to_string(value - 1);
}

AbiFunctionRecord owned_terminal_record(const std::string & terminal)
{
  AbiFunctionRecord record;
  if(terminal == "operator-call" || terminal == "call") {
    record.kind = ABI_FUNCTION_RECORD_OPERATOR_TERMINAL;
    record.terminal = "call";
  } else if(terminal.compare(0, 12, "constructor-") == 0 ||
            terminal.compare(0, 10, "destructor-") == 0) {
    record.kind = ABI_FUNCTION_RECORD_TERMINAL;
    record.terminal = terminal;
  } else {
    record.kind = ABI_FUNCTION_RECORD_TERMINAL_SOURCE;
    record.terminal = terminal;
  }
  return record;
}

bool needs_nested_name(const std::vector<std::string> & components)
{
  if(components.size() == 1) return false;
  return !(components.size() == 2 && components[0] == "std");
}

std::vector<std::string> split_fact_line(const std::string & line)
{
  std::istringstream input(line);
  std::vector<std::string> words;
  std::string word;
  while(input >> word) {
    words.push_back(word);
  }
  return words;
}

void append_stream_case(const AbiFactCase & fact_case, std::string * output,
                        std::string * first_error)
{
  try {
    *output += mangle_fact_case(fact_case);
    *output += "\n";
  } catch(const std::exception & error) {
    if(first_error->empty()) {
      *first_error = error.what();
    }
  }
}

}  // namespace

std::string Mangler::mangle_prefix_chain(
  const std::string & qualified_name, bool register_last,
  const std::vector<std::string> & abi_tags)
{
  const std::vector<std::string> components = name_components(qualified_name);
  const bool standard = components[0] == "std";
  const std::size_t first_prefix = standard ? 1 : 0;
  std::string result;
  if(standard) {
    result = "St";
  }
  std::size_t substitution_end = components.size();
  std::string substitution;
  if(components.size() > first_prefix + 1) {
    for(std::size_t i = components.size() - 2; i >= first_prefix; --i) {
      if(substitutions_.lookup(joined_components(components, i), &substitution)) {
        substitution_end = i;
        break;
      }
      if(i == first_prefix) break;
    }
  }
  if(substitution_end != components.size()) {
    result.clear();
    result += substitution;
  }
  for(std::size_t i = first_prefix; i + 1 < components.size(); ++i) {
    if(substitution_end != components.size() && i <= substitution_end) continue;
    result += source_name(components[i]);
    substitutions_.add(joined_components(components, i));
  }
  result += source_name(components.back());
  if(!abi_tags.empty()) {
    result += mangle_tag_list(abi_tags);
  }
  if(register_last) {
    substitutions_.add(normalized_name(qualified_name));
  }
  return result;
}

std::string Mangler::mangle_qualified_name(
  const std::string & qualified_name, bool register_last,
  const std::vector<std::string> & abi_tags)
{
  const std::vector<std::string> components = name_components(qualified_name);
  const bool nested = needs_nested_name(components);
  const std::string body = mangle_prefix_chain(qualified_name, register_last,
                                               abi_tags);
  return nested ? "N" + body + "E" : body;
}

std::string Mangler::mangle_function_terminal(const AbiFunctionRecord & record)
{
  if(record.kind == ABI_FUNCTION_RECORD_TERMINAL_SOURCE) {
    if(is_dash(record.terminal)) {
      throw std::logic_error("empty function terminal source");
    }
    return source_name(record.terminal);
  }
  if(record.kind == ABI_FUNCTION_RECORD_CONVERSION_TERMINAL) {
    return "cv" + mangle_type(record.type);
  }
  if(record.kind == ABI_FUNCTION_RECORD_OPERATOR_TERMINAL) {
    if(record.terminal == "literal") {
      return "li" + source_name(record.literal_suffix);
    }
    return operator_code(record.terminal);
  }
  if(record.kind == ABI_FUNCTION_RECORD_TERMINAL) {
    if(record.terminal == "constructor-complete") return "C1";
    if(record.terminal == "constructor-base") return "C2";
    if(record.terminal == "destructor-deleting") return "D0";
    if(record.terminal == "destructor-complete") return "D1";
    if(record.terminal == "destructor-base") return "D2";
    throw std::logic_error("unknown ABI function terminal '" +
      record.terminal + "'");
  }
  throw std::logic_error("invalid ABI function terminal record");
}

std::string Mangler::mangle_function_name(
  const std::vector<AbiFunctionRecord> & records,
  std::vector<std::string> * template_arguments,
  bool * has_template_encoding)
{
  struct NamePiece
  {
    const AbiFunctionRecord * record;
    bool template_name;
  };

  std::vector<NamePiece> pieces;
  const AbiFunctionRecord * terminal = 0;
  std::vector<AbiFunctionQualifier> qualifiers;
  std::vector<std::string> tags;
  std::vector<std::string> template_prefixes;
  const AbiFunctionRecord * local_context = 0;
  const AbiFunctionRecord * lambda_context = 0;
  const AbiFunctionRecord * namespace_lambda_context = 0;
  bool standard = false;
  std::string standard_substitution;
  if(has_template_encoding) *has_template_encoding = false;

  for(std::size_t i = 0; i < records.size(); ++i) {
    const AbiFunctionRecord & record = records[i];
    switch(record.kind) {
    case ABI_FUNCTION_RECORD_NAME_SOURCE:
      if(!is_dash(record.source_name)) {
        NamePiece piece = {&record, false};
        pieces.push_back(piece);
      }
      break;
    case ABI_FUNCTION_RECORD_NAME_STD:
      standard = true;
      standard_substitution = record.standard_substitution;
      break;
    case ABI_FUNCTION_RECORD_NAME_TEMPLATE: {
      NamePiece piece = {&record, true};
      pieces.push_back(piece);
      break;
    }
    case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT:
      if(template_arguments) {
        template_arguments->push_back(record.substitution);
      }
      if(has_template_encoding) *has_template_encoding = true;
      break;
    case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX:
      if(!is_dash(record.substitution)) {
        template_prefixes.push_back(record.substitution);
      }
      break;
    case ABI_FUNCTION_RECORD_LOCAL_CONTEXT:
      local_context = &record;
      break;
    case ABI_FUNCTION_RECORD_LAMBDA_CONTEXT:
      lambda_context = &record;
      break;
    case ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT:
      namespace_lambda_context = &record;
      break;
    case ABI_FUNCTION_RECORD_QUALIFIER:
      qualifiers.insert(qualifiers.end(), record.qualifiers.begin(),
                        record.qualifiers.end());
      break;
    case ABI_FUNCTION_RECORD_ABI_TAG:
      tags.push_back(record.name);
      break;
    case ABI_FUNCTION_RECORD_TERMINAL_SOURCE:
    case ABI_FUNCTION_RECORD_TERMINAL:
    case ABI_FUNCTION_RECORD_OPERATOR_TERMINAL:
    case ABI_FUNCTION_RECORD_CONVERSION_TERMINAL:
      if(terminal) {
        throw std::logic_error("multiple ABI function terminals");
      }
      terminal = &record;
      break;
    default:
      break;
    }
  }

  if(local_context || lambda_context || namespace_lambda_context) {
    return mangle_context_function_name(records, template_arguments,
                                        has_template_encoding);
  }

  // The source-level "operator" component and the dash component are
  // placeholders when a terminal record supplies the ABI unqualified name.
  if(terminal && !pieces.empty() && !pieces.back().template_name &&
     (pieces.back().record->source_name == "operator" ||
      is_dash(pieces.back().record->source_name))) {
    pieces.pop_back();
  }
  if(pieces.empty() && !terminal) {
    throw std::logic_error("ABI function has no name");
  }

  const std::size_t component_count = (standard ? 1 : 0) + pieces.size() +
    (terminal ? 1 : 0);
  const bool nested = component_count > 1 &&
    !(standard && component_count == 2 && pieces.size() == 1 && !terminal);

  std::string result;
  if(nested) result += "N";
  bool has_volatile = false;
  bool has_const = false;
  bool has_lvalue_ref = false;
  bool has_rvalue_ref = false;
  for(std::size_t i = 0; i < qualifiers.size(); ++i) {
    has_volatile = has_volatile ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_VOLATILE;
    has_const = has_const || qualifiers[i] == ABI_FUNCTION_QUALIFIER_CONST;
    has_lvalue_ref = has_lvalue_ref ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE;
    has_rvalue_ref = has_rvalue_ref ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE;
  }
  if(has_volatile) result += "V";
  if(has_const) result += "K";
  if(has_lvalue_ref) result += "R";
  if(has_rvalue_ref) result += "O";
  if(standard) {
    result += standard_substitution.empty() ? "St" : standard_substitution;
  }

  const std::size_t final_piece = terminal ? pieces.size() :
    pieces.size() - 1;
  if(!terminal && pieces[final_piece].template_name && has_template_encoding) {
    *has_template_encoding = true;
  }
  for(std::size_t i = 0; i < pieces.size(); ++i) {
    if(!terminal && i == final_piece) break;
    const AbiFunctionRecord & record = *pieces[i].record;
    if(pieces[i].template_name) {
      if(!is_dash(record.substitution)) substitutions_.add(record.substitution);
      if(!is_dash(record.standard_substitution) &&
         !record.standard_substitution.empty()) {
        result += record.standard_substitution;
        if(!record.standard_substitution_includes_arguments) {
          result += mangle_template_args(record.argument_refs);
        }
      } else {
        result += source_name(record.name);
        result += mangle_template_args(record.argument_refs);
      }
      if(!is_dash(record.complete_substitution)) {
        substitutions_.add(record.complete_substitution);
      }
    } else {
      result += source_name(record.source_name);
      if(!is_dash(record.substitution)) substitutions_.add(record.substitution);
    }
  }

  if(terminal) {
    result += mangle_function_terminal(*terminal);
  } else {
    const AbiFunctionRecord & record = *pieces[final_piece].record;
    if(pieces[final_piece].template_name) {
      if(!is_dash(record.standard_substitution) &&
         !record.standard_substitution.empty()) {
        result += record.standard_substitution;
        if(!record.standard_substitution_includes_arguments) {
          result += mangle_template_args(record.argument_refs);
        }
      } else {
        result += source_name(record.name);
        result += mangle_template_args(record.argument_refs);
      }
      if(!is_dash(record.complete_substitution)) {
        substitutions_.add(record.complete_substitution);
      }
    } else {
      result += source_name(record.source_name);
      if(!is_dash(record.substitution)) substitutions_.add(record.substitution);
    }
  }
  if(!tags.empty()) result += mangle_tag_list(tags);
  for(std::size_t i = 0; i < template_prefixes.size(); ++i) {
    substitutions_.add(template_prefixes[i]);
  }
  if(template_arguments && !template_arguments->empty()) {
    result += mangle_template_args(*template_arguments);
  }
  if(nested) result += "E";
  return result;
}

std::string Mangler::mangle_context(const std::string & ref)
{
  const AbiDefinitionRecord * record = definition(ref);
  if(!record || record->kind != ABI_DEFINITION_CONTEXT) {
    throw std::logic_error("unknown ABI context '" + ref + "'");
  }
  if(record->context.kind == ABI_CONTEXT_RAW) {
    return record->context.fragment;
  }
  AbiFunctionShape shape;
  shape.target = record->context.function;
  const std::string encoding = mangle_function(shape);
  if(encoding.compare(0, 2, "_Z") != 0) {
    throw std::logic_error("ABI context function is not Itanium-mangled");
  }
  return "Z" + encoding.substr(2) + "E";
}

std::string Mangler::mangle_local_discriminator(
  const std::string & discriminator) const
{
  return local_discriminator_suffix(discriminator);
}

std::string Mangler::mangle_context_function_name(
  const std::vector<AbiFunctionRecord> & records,
  std::vector<std::string> * template_arguments,
  bool * has_template_encoding)
{
  const AbiFunctionRecord * context = 0;
  const AbiFunctionRecord * terminal = 0;
  std::vector<AbiFunctionQualifier> qualifiers;
  std::vector<std::string> tags;
  std::vector<std::string> template_prefixes;
  enum ContextKind { CONTEXT_LOCAL, CONTEXT_LAMBDA, CONTEXT_NAMESPACE_LAMBDA };
  ContextKind context_kind = CONTEXT_LOCAL;
  std::size_t context_count = 0;
  if(has_template_encoding) *has_template_encoding = false;

  for(std::size_t i = 0; i < records.size(); ++i) {
    const AbiFunctionRecord & record = records[i];
    if(record.kind == ABI_FUNCTION_RECORD_LOCAL_CONTEXT ||
       record.kind == ABI_FUNCTION_RECORD_LAMBDA_CONTEXT ||
       record.kind == ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT) {
      ++context_count;
      context = &record;
      context_kind = record.kind == ABI_FUNCTION_RECORD_LOCAL_CONTEXT ?
        CONTEXT_LOCAL : record.kind == ABI_FUNCTION_RECORD_LAMBDA_CONTEXT ?
        CONTEXT_LAMBDA : CONTEXT_NAMESPACE_LAMBDA;
    } else if(record.kind == ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT) {
      if(template_arguments) template_arguments->push_back(record.substitution);
      if(has_template_encoding) *has_template_encoding = true;
    } else if(record.kind == ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX) {
      if(!is_dash(record.substitution)) template_prefixes.push_back(record.substitution);
    } else if(record.kind == ABI_FUNCTION_RECORD_QUALIFIER) {
      qualifiers.insert(qualifiers.end(), record.qualifiers.begin(),
                        record.qualifiers.end());
    } else if(record.kind == ABI_FUNCTION_RECORD_ABI_TAG) {
      tags.push_back(record.name);
    } else if(record.kind == ABI_FUNCTION_RECORD_TERMINAL_SOURCE ||
              record.kind == ABI_FUNCTION_RECORD_TERMINAL ||
              record.kind == ABI_FUNCTION_RECORD_OPERATOR_TERMINAL ||
              record.kind == ABI_FUNCTION_RECORD_CONVERSION_TERMINAL) {
      if(terminal) throw std::logic_error("multiple ABI function terminals");
      terminal = &record;
    }
  }
  if(context_count != 1 || !context) {
    throw std::logic_error("ABI function needs exactly one local context");
  }

  std::string result;
  if(context_kind == CONTEXT_NAMESPACE_LAMBDA) {
    result = "N";
    append_qualifier_codes(qualifiers, &result);
    for(std::size_t i = 0; i < context->namespace_qualifiers.size(); ++i) {
      result += source_name(context->namespace_qualifiers[i]);
    }
    result += source_name(context->source_name);
  } else {
    result = mangle_context(context->context_ref);
    result += "N";
    append_qualifier_codes(qualifiers, &result);
    if(context_kind == CONTEXT_LOCAL) {
      result += source_name(context->source_name);
      result += local_discriminator_suffix(context->discriminator);
    } else {
      const std::string discriminator = context->discriminator.empty() ?
        context->source_name : context->discriminator;
      result += "Ul";
      if(context->types.empty()) {
        result += "v";
      } else {
        for(std::size_t i = 0; i < context->types.size(); ++i) {
          result += mangle_type(context->types[i]);
        }
      }
      result += "E" + discriminator + "_";
    }
  }
  if(terminal) {
    result += mangle_function_terminal(*terminal);
  } else {
    result += "cl";
  }
  if(!tags.empty()) result += mangle_tag_list(tags);
  for(std::size_t i = 0; i < template_prefixes.size(); ++i) {
    substitutions_.add(template_prefixes[i]);
  }
  if(template_arguments && !template_arguments->empty()) {
    result += mangle_template_args(*template_arguments);
  }
  return result + "E";
}

std::string Mangler::mangle_path_name(
  const AbiFunctionTarget & target,
  const std::vector<AbiFunctionRecord> & records,
  const std::vector<std::string> & template_arguments)
{
  const std::vector<std::string> components = name_components(
    target.qualified_name);
  const bool nested = needs_nested_name(components);
  const bool standard = components[0] == "std";
  const std::size_t first_prefix = standard ? 1 : 0;
  const AbiFunctionRecord * terminal = 0;
  std::vector<std::string> tags;
  std::vector<AbiFunctionQualifier> qualifiers;
  for(std::size_t i = 0; i < records.size(); ++i) {
    const AbiFunctionRecord & record = records[i];
    if(record.kind == ABI_FUNCTION_RECORD_ABI_TAG) {
      tags.push_back(record.name);
    } else if(record.kind == ABI_FUNCTION_RECORD_QUALIFIER) {
      qualifiers.insert(qualifiers.end(), record.qualifiers.begin(),
                        record.qualifiers.end());
    } else if(record.kind == ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX) {
      if(!is_dash(record.substitution)) substitutions_.add(record.substitution);
    } else if(record.kind == ABI_FUNCTION_RECORD_TERMINAL_SOURCE ||
              record.kind == ABI_FUNCTION_RECORD_TERMINAL ||
              record.kind == ABI_FUNCTION_RECORD_OPERATOR_TERMINAL ||
              record.kind == ABI_FUNCTION_RECORD_CONVERSION_TERMINAL) {
      if(terminal) {
        throw std::logic_error("multiple ABI function terminals");
      }
      terminal = &record;
    }
  }

  std::string prefix_result;
  if(standard) prefix_result = "St";
  std::size_t substitution_end = components.size();
  std::string substitution;
  if(components.size() > first_prefix + 1) {
    for(std::size_t i = components.size() - 2; i >= first_prefix; --i) {
      if(substitutions_.lookup(joined_components(components, i), &substitution)) {
        substitution_end = i;
        break;
      }
      if(i == first_prefix) break;
    }
  }
  if(substitution_end != components.size()) {
    prefix_result = substitution;
  }
  for(std::size_t i = first_prefix; i + 1 < components.size(); ++i) {
    if(substitution_end != components.size() && i <= substitution_end) continue;
    prefix_result += source_name(components[i]);
    substitutions_.add(joined_components(components, i));
  }

  std::string result;
  if(nested) result += "N";
  bool has_volatile = false;
  bool has_const = false;
  bool has_lvalue_ref = false;
  bool has_rvalue_ref = false;
  for(std::size_t i = 0; i < qualifiers.size(); ++i) {
    has_volatile = has_volatile ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_VOLATILE;
    has_const = has_const || qualifiers[i] == ABI_FUNCTION_QUALIFIER_CONST;
    has_lvalue_ref = has_lvalue_ref ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE;
    has_rvalue_ref = has_rvalue_ref ||
      qualifiers[i] == ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE;
  }
  if(has_volatile) result += "V";
  if(has_const) result += "K";
  if(has_lvalue_ref) result += "R";
  if(has_rvalue_ref) result += "O";
  result += prefix_result;
  if(terminal) {
    result += mangle_function_terminal(*terminal);
  } else {
    result += source_name(components.back());
  }
  if(!tags.empty()) result += mangle_tag_list(tags);
  if(!template_arguments.empty()) {
    result += mangle_template_args(template_arguments);
    substitutions_.add(normalized_name(target.qualified_name));
  }
  if(nested) result += "E";
  return result;
}

std::string Mangler::mangle_function_path(const AbiFunctionShape & shape)
{
  const AbiFunctionTarget & target = shape.target;
  std::vector<std::string> template_arguments;
  std::vector<AbiType> path_parameters;
  bool path_variadic = false;
  for(std::size_t i = 0; i < target.path_operands.size(); ++i) {
    const AbiFunctionPathOperand & operand = target.path_operands[i];
    if(operand.kind == ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT) {
      if(argument_definition(operand.argument_ref)) {
        template_arguments.push_back(operand.argument_ref);
      } else if(type_definition(operand.argument_ref)) {
        AbiType type;
        type.kind = ABI_TYPE_NAME_OR_REFERENCE;
        type.name = operand.argument_ref;
        path_parameters.push_back(type);
      } else {
        throw std::logic_error("unknown ABI path operand '" +
          operand.argument_ref + "'");
      }
    } else if(operand.kind == ABI_FUNCTION_PATH_TYPE) {
      path_parameters.push_back(operand.type);
    } else if(operand.kind == ABI_FUNCTION_PATH_VARIADIC) {
      path_variadic = true;
    }
  }

  std::vector<AbiType> parameters;
  std::vector<AbiType> results;
  bool explicit_parameters = false;
  bool variadic = path_variadic;
  for(std::size_t i = 0; i < shape.records.size(); ++i) {
    const AbiFunctionRecord & record = shape.records[i];
    if(record.kind == ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT) {
      template_arguments.push_back(record.substitution);
    } else if(record.kind == ABI_FUNCTION_RECORD_PARAMETER) {
      explicit_parameters = true;
      parameters.push_back(record.type);
    } else if(record.kind == ABI_FUNCTION_RECORD_RESULT) {
      results.push_back(record.type);
    } else if(record.kind == ABI_FUNCTION_RECORD_VARIADIC) {
      variadic = true;
    }
  }
  if(!explicit_parameters) parameters = path_parameters;

  std::string result = "_Z" + mangle_path_name(target, shape.records,
                                                 template_arguments);
  if(!template_arguments.empty()) {
    for(std::size_t i = 0; i < results.size(); ++i) {
      result += mangle_type(results[i]);
    }
  }
  if(parameters.empty()) {
    result += "v";
  } else {
    for(std::size_t i = 0; i < parameters.size(); ++i) {
      result += mangle_type(parameters[i]);
    }
  }
  if(variadic) result += "z";
  return result;
}

std::string Mangler::mangle_function_encoding(const AbiFunctionShape & shape)
{
  std::vector<std::string> template_arguments;
  bool has_template_encoding = false;
  const std::string name = mangle_function_name(shape.records,
                                                &template_arguments,
                                                &has_template_encoding);
  std::vector<AbiType> parameters;
  std::vector<AbiType> results;
  bool variadic = false;
  for(std::size_t i = 0; i < shape.records.size(); ++i) {
    const AbiFunctionRecord & record = shape.records[i];
    if(record.kind == ABI_FUNCTION_RECORD_PARAMETER) {
      parameters.push_back(record.type);
    } else if(record.kind == ABI_FUNCTION_RECORD_RESULT) {
      results.push_back(record.type);
    } else if(record.kind == ABI_FUNCTION_RECORD_VARIADIC) {
      variadic = true;
    }
  }
  std::string result = "_Z" + name;
  if(has_template_encoding) {
    for(std::size_t i = 0; i < results.size(); ++i) {
      result += mangle_type(results[i]);
    }
  }
  if(parameters.empty()) {
    result += "v";
  } else {
    for(std::size_t i = 0; i < parameters.size(); ++i) {
      result += mangle_type(parameters[i]);
    }
  }
  if(variadic) result += "z";
  return result;
}

std::string Mangler::mangle_owned_function(const AbiFunctionShape & shape)
{
  AbiFunctionShape owned = shape;
  owned.target.kind = ABI_FUNCTION_TARGET_ENCODING;
  AbiFunctionRecord context;
  if(shape.target.kind == ABI_FUNCTION_TARGET_LOCAL) {
    context.kind = ABI_FUNCTION_RECORD_LOCAL_CONTEXT;
    context.context_ref = shape.target.context_ref;
    context.source_name = shape.target.source_name;
    context.discriminator = shape.target.discriminator;
  } else if(shape.target.kind == ABI_FUNCTION_TARGET_LAMBDA) {
    context.kind = ABI_FUNCTION_RECORD_LAMBDA_CONTEXT;
    context.context_ref = shape.target.context_ref;
    context.discriminator = shape.target.discriminator;
    context.types = shape.target.signature_parameter_types;
  } else {
    context.kind = ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT;
    context.source_name = shape.target.source_name;
    context.namespace_qualifiers = shape.target.namespace_qualifiers;
  }
  owned.records.insert(owned.records.begin(), context);
  owned.records.insert(owned.records.begin() + 1,
                       owned_terminal_record(shape.target.terminal));
  return mangle_function_encoding(owned);
}

std::string Mangler::mangle_function(const AbiFunctionShape & shape)
{
  if(shape.target.kind == ABI_FUNCTION_TARGET_PATH) {
    return mangle_function_path(shape);
  }
  if(shape.target.kind == ABI_FUNCTION_TARGET_ENCODING) {
    return mangle_function_encoding(shape);
  }
  if(shape.target.kind == ABI_FUNCTION_TARGET_LOCAL ||
     shape.target.kind == ABI_FUNCTION_TARGET_LAMBDA ||
     shape.target.kind == ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA) {
    return mangle_owned_function(shape);
  }
  throw std::logic_error("unsupported function target");
}

std::string Mangler::mangle_call_offset(long long offset) const
{
  return "h" + signed_number(offset) + "_";
}

std::string Mangler::mangle_internal_name(const std::string & qualified_name)
{
  const std::vector<std::string> components = name_components(qualified_name);
  if(components.size() == 1) {
    return "L" + source_name(components[0]);
  }
  std::string result = "N";
  for(std::size_t i = 0; i + 1 < components.size(); ++i) {
    result += source_name(components[i]);
  }
  result += "L" + source_name(components.back()) + "E";
  return result;
}

std::string Mangler::mangle_special_target(const AbiTargetRecord & target)
{
  if(target.kind == ABI_TARGET_FACT_TYPEINFO) {
    return "_ZTI" + mangle_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VTABLE) {
    return "_ZTV" + mangle_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VTT) {
    return "_ZTT" + mangle_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_CONSTRUCTION_VTABLE) {
    return "_ZTC" + mangle_type(target.type) + std::to_string(target.base_offset) +
      "_" + mangle_type(target.base_type);
  }
  if(target.kind == ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER) {
    return "_ZTW" + mangle_qualified_name(target.qualified_name, false);
  }
  if(target.kind == ABI_TARGET_FACT_VARIABLE) {
    return "_Z" + mangle_qualified_name(target.qualified_name, false);
  }
  if(target.kind == ABI_TARGET_FACT_TYPE) {
    return mangle_type(target.type);
  }
  throw std::logic_error("unsupported target");
}

std::string Mangler::mangle_target(const AbiTargetRecord & target)
{
  AbiFunctionShape shape;
  if(target.kind == ABI_TARGET_FACT_FUNCTION ||
     target.kind == ABI_TARGET_FACT_THUNK ||
     target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
    shape.target = target.function;
  }
  return mangle_target(target, shape);
}

std::string Mangler::mangle_target(const AbiTargetRecord & target,
                                   const AbiFunctionShape & shape)
{
  if(target.kind == ABI_TARGET_FACT_FUNCTION) {
    if(target.c_linkage) return target.function.qualified_name;
    return mangle_function(shape);
  }
  if(target.kind == ABI_TARGET_FACT_THUNK ||
     target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
    const std::string base = mangle_function(shape);
    if(base.compare(0, 2, "_Z") != 0) {
      throw std::logic_error("thunk function is not Itanium-mangled");
    }
    const std::string encoding = base.substr(2);
    if(target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
      return "_ZTv0_" + signed_number(target.vcall_offset) + "_" + encoding;
    }
    std::string result = "_ZT";
    if(target.has_result_adjust) {
      result += "c";
    }
    result += mangle_call_offset(target.this_adjust);
    if(target.has_result_adjust) {
      if(target.result_adjust_virtual) {
        result += "v0_" + signed_number(target.result_vcall_offset) + "_";
      } else {
        result += mangle_call_offset(target.result_adjust);
      }
    }
    return result + encoding;
  }
  return mangle_special_target(target);
}

std::string mangle_fact_case(const AbiFactCase & fact_case)
{
  AbiDefinitionTable definitions;
  const AbiTargetRecord * target = 0;
  AbiFunctionShape shape;
  for(std::size_t i = 0; i < fact_case.records.size(); ++i) {
    const AbiFactRecord & record = fact_case.records[i];
    if(record.kind == ABI_FACT_RECORD_DEFINITION) {
      definitions.add(record.definition);
    } else if(record.kind == ABI_FACT_RECORD_TARGET) {
      if(target) {
        throw std::logic_error("ABI case must contain exactly one target");
      }
      target = &record.target;
    } else if(record.kind == ABI_FACT_RECORD_FUNCTION) {
      shape.records.push_back(record.function);
    }
  }
  if(!target) {
    throw std::logic_error("ABI case must contain exactly one target");
  }
  shape.target = target->function;
  return abi_mangle::Mangler(definitions).mangle_target(*target, shape);
}

std::string mangle_fact_files(const std::vector<std::string> & input_paths)
{
  std::string output;
  std::string first_error;
  for(std::size_t i = 0; i < input_paths.size(); ++i) {
    std::ifstream input(input_paths[i].c_str());
    if(!input) {
      throw std::logic_error("unable to read ABI fact file '" + input_paths[i] +
        "'");
    }
    AbiFactCase current;
    bool active = false;
    std::string line;
    while(std::getline(input, line)) {
      const std::vector<std::string> words = split_fact_line(line);
      if(words.empty()) {
        continue;
      }
      if(words[0] == "case") {
        if(words.size() != 2) {
          throw std::logic_error("malformed case header");
        }
        if(active) {
          append_stream_case(current, &output, &first_error);
        }
        current = AbiFactCase();
        current.label = words[1];
        active = true;
        continue;
      }
      if(!active) {
        active = true;
        current = AbiFactCase();
      }
      current.records.push_back(parse_fact_record_words(words));
    }
    if(active) {
      append_stream_case(current, &output, &first_error);
    } else {
      throw std::logic_error("ABI fact file contains no cases");
    }
  }
  if(!first_error.empty()) {
    throw std::logic_error(first_error);
  }
  return output;
}

}  // namespace abi_mangle
