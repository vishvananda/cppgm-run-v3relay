#include "abi_mangle.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace abi_mangle {
namespace {

typedef std::vector<std::string> Words;

struct ParsedType
{
  AbiType type;
  std::size_t next = 0;
};

std::vector<std::string> split_words(const std::string & line)
{
  std::istringstream input(line);
  std::vector<std::string> words;
  std::string word;
  while(input >> word) {
    words.push_back(word);
  }
  return words;
}

void require_words(const Words & words, std::size_t count,
                   const std::string & what)
{
  if(words.size() < count) {
    throw std::logic_error("malformed " + what);
  }
}

void require_end(const Words & words, std::size_t next,
                 const std::string & what)
{
  if(next != words.size()) {
    throw std::logic_error("extra words in " + what);
  }
}

std::size_t decimal_index(const std::string & word)
{
  if(word.empty() || word[0] == '-') {
    throw std::logic_error("ABI fact index must be decimal in '" + word + "'");
  }
  std::size_t value = 0;
  const std::size_t limit = std::numeric_limits<std::size_t>::max();
  for(std::size_t i = 0; i < word.size(); ++i) {
    if(word[i] < '0' || word[i] > '9') {
      throw std::logic_error("ABI fact index must be decimal in '" + word + "'");
    }
    const std::size_t digit = static_cast<std::size_t>(word[i] - '0');
    if(value > (limit - digit) / 10) {
      throw std::logic_error("ABI fact index is too large");
    }
    value = value * 10 + digit;
  }
  return value;
}

long long signed_value(const std::string & word)
{
  char * end = 0;
  const char * start = word.c_str();
  const long long value = std::strtoll(start, &end, 10);
  if(end == start || *end != '\0') {
    throw std::logic_error("ABI fact value must be decimal in '" + word + "'");
  }
  if(value == std::numeric_limits<long long>::min() && word !=
      "-9223372036854775808") {
    throw std::logic_error("ABI fact value is out of range");
  }
  return value;
}

bool fact_flag(const std::string & word)
{
  if(word == "yes" || word == "true" || word == "1") {
    return true;
  }
  if(word == "no" || word == "false" || word == "0") {
    return false;
  }
  throw std::logic_error("ABI fact flag must be yes/no in '" + word + "'");
}

bool builtin_word(const std::string & word)
{
  static const char * const names[] = {
    "void", "bool", "char", "schar", "uchar", "short", "ushort", "int",
    "uint", "long", "ulong", "longlong", "ulonglong", "int128", "uint128",
    "float", "double", "longdouble", "float128", "wchar", "char16", "char32",
    "nullptr", "auto", "complex-float", "complex-double", "complex-longdouble"
  };
  for(std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if(word == names[i]) {
      return true;
    }
  }
  return false;
}

AbiType builtin_type(const std::string & word)
{
  AbiType type;
  type.kind = ABI_TYPE_BUILTIN;
  type.name = word;
  return type;
}

std::size_t memberptr_separator(const std::string & text)
{
  for(std::size_t i = 0; i < text.size(); ++i) {
    if(text[i] != ':' || (i > 0 && text[i - 1] == ':') ||
       (i + 1 < text.size() && text[i + 1] == ':')) {
      continue;
    }
    return i;
  }
  throw std::logic_error("memberptr type needs an owner and member type");
}

AbiType compact_type(const std::string & word)
{
  if(builtin_word(word)) {
    return builtin_type(word);
  }
  const std::size_t colon = word.find(':');
  if(colon == std::string::npos || (colon + 1 < word.size() &&
                                    word[colon + 1] == ':')) {
    AbiType type;
    type.kind = ABI_TYPE_NAME_OR_REFERENCE;
    type.name = word;
    return type;
  }
  const std::string head = word.substr(0, colon);
  const std::string tail = word.substr(colon + 1);
  if(tail.empty()) {
    throw std::logic_error("empty compact ABI type operand");
  }
  if(head == "named") {
    AbiType type;
    type.kind = ABI_TYPE_NAMED;
    type.name = tail;
    return type;
  }
  if(head == "ptr" || head == "ref" || head == "rref" || head == "const" ||
     head == "volatile" || head == "pack") {
    AbiType type = compact_type(tail);
    if(head == "ptr") {
      AbiType wrapped;
      wrapped.kind = ABI_TYPE_POINTER;
      wrapped.types.push_back(type);
      return wrapped;
    }
    if(head == "ref" || head == "rref") {
      AbiType wrapped;
      wrapped.kind = head == "ref" ? ABI_TYPE_LVALUE_REFERENCE :
        ABI_TYPE_RVALUE_REFERENCE;
      wrapped.lvalue_ref = head == "ref";
      wrapped.rvalue_ref = head == "rref";
      wrapped.types.push_back(type);
      return wrapped;
    }
    if(head == "pack") {
      AbiType wrapped;
      wrapped.kind = ABI_TYPE_PACK_EXPANSION;
      wrapped.types.push_back(type);
      return wrapped;
    }
    type.is_const = type.is_const || head == "const";
    type.is_volatile = type.is_volatile || head == "volatile";
    return type;
  }
  if(head == "array") {
    const std::size_t bound_end = tail.find(':');
    if(bound_end == std::string::npos) {
      throw std::logic_error("array type needs a bound");
    }
    AbiType type;
    type.kind = ABI_TYPE_ARRAY;
    type.array_bound.kind = ABI_ARRAY_BOUND_VALUE;
    type.array_bound.value = tail.substr(0, bound_end);
    decimal_index(type.array_bound.value);
    type.types.push_back(compact_type(tail.substr(bound_end + 1)));
    return type;
  }
  if(head == "memberptr") {
    const std::size_t split = memberptr_separator(tail);
    AbiType type;
    type.kind = ABI_TYPE_MEMBER_POINTER;
    type.types.push_back(compact_type(tail.substr(0, split)));
    type.types.push_back(compact_type(tail.substr(split + 1)));
    return type;
  }
  throw std::logic_error("unknown compact ABI type '" + word + "'");
}

ParsedType parse_type_at(const Words & words, std::size_t pos);

ParsedType parse_type_sequence(const Words & words, std::size_t pos,
                               std::vector<AbiType> * output)
{
  ParsedType result;
  result.next = pos;
  while(result.next < words.size()) {
    const ParsedType item = parse_type_at(words, result.next);
    output->push_back(item.type);
    result.next = item.next;
  }
  return result;
}

ParsedType parse_type_at(const Words & words, std::size_t pos)
{
  require_words(words, pos + 1, "type");
  const std::string & head = words[pos];
  ParsedType result;
  result.next = pos + 1;
  if(head == "vendor" || head == "builtin-transform") {
    require_words(words, pos + 3, "type");
    result.type.kind = head == "vendor" ? ABI_TYPE_VENDOR_QUALIFIED :
      ABI_TYPE_BUILTIN_TRANSFORM;
    result.type.name = words[pos + 1];
    const ParsedType child = parse_type_at(words, pos + 2);
    result.type.types.push_back(child.type);
    result.next = child.next;
    return result;
  }
  if(head == "function-type" || head == "function-type-variadic") {
    require_words(words, pos + 2, "function type");
    result.type.kind = ABI_TYPE_FUNCTION;
    result.type.variadic = head == "function-type-variadic";
    const ParsedType sequence = parse_type_sequence(words, pos + 1,
                                                    &result.type.types);
    result.next = sequence.next;
    return result;
  }
  if(head == "member-pointer") {
    require_words(words, pos + 3, "member-pointer type");
    const ParsedType owner = parse_type_at(words, pos + 1);
    const ParsedType member = parse_type_at(words, owner.next);
    result.type.kind = ABI_TYPE_MEMBER_POINTER;
    result.type.types.push_back(owner.type);
    result.type.types.push_back(member.type);
    result.next = member.next;
    return result;
  }
  if(head == "template" || head == "std-template") {
    const bool standard = head == "std-template";
    const std::size_t fixed = standard ? pos + 4 : pos + 2;
    require_words(words, fixed, "template type");
    result.type.kind = standard ? ABI_TYPE_STD_TEMPLATE_SPECIALIZATION :
      ABI_TYPE_TEMPLATE_SPECIALIZATION;
    if(standard) {
      result.type.standard_substitution = words[pos + 1];
      result.type.standard_substitution_includes_arguments =
        fact_flag(words[pos + 2]);
      result.type.name = words[pos + 3];
      result.next = pos + 4;
    } else {
      result.type.name = words[pos + 1];
      result.next = pos + 2;
    }
    while(result.next < words.size()) {
      result.type.argument_refs.push_back(words[result.next++]);
    }
    return result;
  }
  if(head == "template-param" || head == "template-param-subst") {
    require_words(words, pos + 2, "template parameter type");
    result.type.kind = ABI_TYPE_TEMPLATE_PARAMETER;
    result.type.index = decimal_index(words[pos + 1]);
    result.type.substitutable = head == "template-param-subst";
    result.next = pos + 2;
    return result;
  }
  if(head == "template-param-template") {
    require_words(words, pos + 2, "template parameter template type");
    result.type.kind = ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION;
    result.type.index = decimal_index(words[pos + 1]);
    result.next = pos + 2;
    while(result.next < words.size()) {
      result.type.argument_refs.push_back(words[result.next++]);
    }
    return result;
  }
  if(head == "name") {
    require_words(words, pos + 2, "named type");
    result.type.kind = ABI_TYPE_NAMED;
    result.type.name = words[pos + 1];
    result.next = pos + 2;
    return result;
  }
  if(head == "decltype") {
    require_words(words, pos + 2, "decltype type");
    result.type.kind = ABI_TYPE_DECLTYPE_EXPRESSION;
    result.type.expression_ref = words[pos + 1];
    result.next = pos + 2;
    return result;
  }
  if(head == "member" || head == "member-template") {
    require_words(words, pos + 3, "member type");
    const ParsedType owner = parse_type_at(words, pos + 1);
    result.type.kind = head == "member" ? ABI_TYPE_MEMBER :
      ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION;
    result.type.types.push_back(owner.type);
    result.next = owner.next;
    require_words(words, result.next + 1, "member type");
    result.type.name = words[result.next++];
    if(head == "member-template") {
      while(result.next < words.size()) {
        result.type.argument_refs.push_back(words[result.next++]);
      }
    }
    return result;
  }
  if(head == "lambda-closure" || head == "local-type") {
    require_words(words, pos + 3, "local ABI type");
    result.type.kind = head == "lambda-closure" ? ABI_TYPE_LAMBDA_CLOSURE :
      ABI_TYPE_LOCAL_TYPE;
    result.type.context_ref = words[pos + 1];
    result.type.name = head == "local-type" ? words[pos + 2] : "";
    result.type.discriminator = words[pos + (head == "local-type" ? 3 : 2)];
    result.next = pos + (head == "local-type" ? 4 : 3);
    if(head == "lambda-closure") {
      parse_type_sequence(words, result.next, &result.type.types);
      result.next = words.size();
    }
    return result;
  }
  if(head == "namespace-lambda") {
    require_words(words, pos + 2, "namespace lambda type");
    result.type.kind = ABI_TYPE_NAMESPACE_LAMBDA;
    result.type.name = words[pos + 1];
    result.next = pos + 2;
    while(result.next < words.size()) {
      result.type.namespace_qualifiers.push_back(words[result.next++]);
    }
    return result;
  }
  if(head == "tagged") {
    require_words(words, pos + 3, "tagged type");
    const ParsedType base = parse_type_at(words, pos + 1);
    result.type = base.type;
    result.type.tagged = true;
    result.type.abi_tags.clear();
    result.next = base.next;
    while(result.next < words.size()) {
      result.type.abi_tags.push_back(words[result.next++]);
    }
    return result;
  }
  if(head == "const" || head == "volatile" || head == "ref" || head == "rref" ||
     head == "pack") {
    require_words(words, pos + 2, "wrapped type");
    const ParsedType child = parse_type_at(words, pos + 1);
    result.type = child.type;
    if(head == "const") {
      result.type.is_const = true;
    } else if(head == "volatile") {
      result.type.is_volatile = true;
    } else {
      AbiType wrapper;
      wrapper.kind = head == "ref" ? ABI_TYPE_LVALUE_REFERENCE :
        head == "rref" ? ABI_TYPE_RVALUE_REFERENCE : ABI_TYPE_PACK_EXPANSION;
      wrapper.lvalue_ref = head == "ref";
      wrapper.rvalue_ref = head == "rref";
      wrapper.types.push_back(child.type);
      result.type = wrapper;
    }
    result.next = child.next;
    return result;
  }
  result.type = compact_type(head);
  return result;
}

AbiFunctionPathOperand path_operand(const Words & words, std::size_t * pos)
{
  AbiFunctionPathOperand operand;
  if(words[*pos] == "variadic") {
    operand.kind = ABI_FUNCTION_PATH_VARIADIC;
    ++*pos;
    return operand;
  }
  const ParsedType parsed = parse_type_at(words, *pos);
  if(parsed.next == *pos + 1 && parsed.type.kind == ABI_TYPE_NAME_OR_REFERENCE) {
    operand.kind = ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT;
    operand.argument_ref = parsed.type.name;
  } else {
    operand.kind = ABI_FUNCTION_PATH_TYPE;
    operand.type = parsed.type;
  }
  *pos = parsed.next;
  return operand;
}

AbiFunctionTarget parse_function_target(const Words & words, std::size_t pos)
{
  require_words(words, pos + 2, "function target");
  AbiFunctionTarget target;
  const std::string & form = words[pos + 1];
  if(form == "encoding") {
    require_end(words, pos + 2, "function target");
    target.kind = ABI_FUNCTION_TARGET_ENCODING;
    return target;
  }
  if(form == "local" || form == "lambda") {
    require_words(words, pos + (form == "local" ? 6 : 5), "function target");
    target.kind = form == "local" ? ABI_FUNCTION_TARGET_LOCAL :
      ABI_FUNCTION_TARGET_LAMBDA;
    target.context_ref = words[pos + 2];
    target.source_name = form == "local" ? words[pos + 3] : "";
    target.terminal = form == "local" ? words[pos + 4] : words[pos + 4];
    target.discriminator = form == "local" ? words[pos + 5] : words[pos + 3];
    std::size_t next = form == "local" ? pos + 6 : pos + 5;
    while(next < words.size()) {
      const ParsedType signature = parse_type_at(words, next);
      target.signature_parameter_types.push_back(signature.type);
      next = signature.next;
    }
    return target;
  }
  if(form == "namespace-lambda") {
    require_words(words, pos + 4, "namespace lambda target");
    target.kind = ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA;
    target.source_name = words[pos + 2];
    target.terminal = words[pos + 3];
    for(std::size_t i = pos + 4; i < words.size(); ++i) {
      target.namespace_qualifiers.push_back(words[i]);
    }
    return target;
  }
  const std::size_t name_pos = form == "path" ? pos + 2 : pos + 1;
  require_words(words, name_pos + 1, "function target");
  target.kind = ABI_FUNCTION_TARGET_PATH;
  target.qualified_name = words[name_pos];
  std::size_t next = name_pos + 1;
  while(next < words.size()) {
    target.path_operands.push_back(path_operand(words, &next));
  }
  return target;
}

AbiDefinitionRecord parse_type_definition(const Words & words)
{
  require_words(words, 3, "type definition");
  AbiDefinitionRecord record;
  record.kind = ABI_DEFINITION_TYPE;
  record.id = words[1];
  const ParsedType parsed = parse_type_at(words, 2);
  require_end(words, parsed.next, "type definition");
  record.type = parsed.type;
  return record;
}

AbiDefinitionRecord parse_argument_definition(const Words & words)
{
  require_words(words, 3, "argument definition");
  AbiDefinitionRecord record;
  record.kind = ABI_DEFINITION_TEMPLATE_ARGUMENT;
  record.id = words[1];
  const std::string & form = words[2];
  if(form == "type") {
    const ParsedType parsed = parse_type_at(words, 3);
    require_end(words, parsed.next, "argument definition");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_TYPE;
    record.template_argument.type = parsed.type;
    return record;
  }
  if(form == "value") {
    require_words(words, 5, "value argument");
    const ParsedType value_type = parse_type_at(words, 3);
    require_end(words, value_type.next + 1, "value argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_VALUE;
    record.template_argument.value_type = value_type.type;
    record.template_argument.value = signed_value(words[value_type.next]);
    return record;
  }
  if(form == "dependent-value") {
    require_words(words, 6, "dependent value argument");
    const ParsedType value_type = parse_type_at(words, 4);
    require_end(words, value_type.next + 1, "dependent value argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE;
    record.template_argument.expression_ref = words[3];
    record.template_argument.value_type = value_type.type;
    record.template_argument.value = signed_value(words[value_type.next]);
    return record;
  }
  if(form == "expression") {
    require_words(words, 4, "expression argument");
    require_end(words, 4, "expression argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_EXPRESSION;
    record.template_argument.expression_ref = words[3];
    return record;
  }
  if(form == "template-param-template") {
    require_words(words, 4, "template parameter argument");
    require_end(words, 4, "template parameter argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE;
    record.template_argument.index = decimal_index(words[3]);
    return record;
  }
  if(form == "template-entity") {
    require_words(words, 4, "template entity argument");
    require_end(words, 4, "template entity argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY;
    record.template_argument.name = words[3];
    return record;
  }
  if(form == "member-template-entity") {
    require_words(words, 6, "member template entity argument");
    require_end(words, 6, "member template entity argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY;
    record.template_argument.type = compact_type(words[3]);
    record.template_argument.name = words[4];
    record.template_argument.substitution = words[5];
    return record;
  }
  if(form == "entity-address") {
    require_words(words, 4, "entity argument");
    require_end(words, 4, "entity argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_ENTITY;
    record.template_argument.entity_ref = words[3];
    record.template_argument.address_of = true;
    return record;
  }
  if(form == "member-external-address") {
    require_words(words, 13, "external member argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY;
    record.template_argument.symbol = words[3];
    record.template_argument.owner_type = compact_type(words[4]);
    record.template_argument.name = words[5];
    record.template_argument.member_is_function = fact_flag(words[6]);
    record.template_argument.member_function_const = fact_flag(words[7]);
    record.template_argument.member_function_volatile = fact_flag(words[8]);
    record.template_argument.member_function_lvalue_ref = fact_flag(words[9]);
    record.template_argument.member_function_rvalue_ref = fact_flag(words[10]);
    record.template_argument.member_function_variadic = fact_flag(words[11]);
    std::size_t next = 12;
    while(next < words.size()) {
      const ParsedType parameter = parse_type_at(words, next);
      record.template_argument.parameter_types.push_back(parameter.type);
      next = parameter.next;
    }
    return record;
  }
  if(form == "untyped-value") {
    require_words(words, 4, "untyped value argument");
    require_end(words, 4, "untyped value argument");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE;
    record.template_argument.value = signed_value(words[3]);
    return record;
  }
  if(form == "pack") {
    require_words(words, 4, "argument pack");
    record.template_argument.kind = ABI_TEMPLATE_ARGUMENT_PACK;
    for(std::size_t i = 3; i < words.size(); ++i) {
      record.template_argument.argument_refs.push_back(words[i]);
    }
    return record;
  }
  throw std::logic_error("unknown ABI argument form '" + form + "'");
}

AbiDefinitionRecord parse_expression_definition(const Words & words)
{
  require_words(words, 4, "expression definition");
  AbiDefinitionRecord record;
  record.kind = ABI_DEFINITION_EXPRESSION;
  record.id = words[1];
  const std::string & form = words[2];
  AbiDependentExpression & expression = record.expression;
  if(form == "template-param" || form == "function-param") {
    require_end(words, 4, "parameter expression");
    expression.kind = form == "template-param" ?
      ABI_EXPRESSION_TEMPLATE_PARAMETER : ABI_EXPRESSION_FUNCTION_PARAMETER;
    expression.index = decimal_index(words[3]);
    return record;
  }
  if(form == "literal" || form == "integral-value") {
    require_end(words, 4, "literal expression");
    expression.kind = form == "literal" ? ABI_EXPRESSION_LITERAL :
      ABI_EXPRESSION_INTEGRAL_VALUE;
    expression.text = words[3];
    expression.value = signed_value(words[3]);
    return record;
  }
  if(form == "unary") {
    require_words(words, 5, "unary expression");
    require_end(words, 5, "unary expression");
    expression.kind = ABI_EXPRESSION_UNARY;
    expression.op = words[3];
    expression.expression_refs.push_back(words[4]);
    return record;
  }
  if(form == "binary") {
    require_words(words, 6, "binary expression");
    require_end(words, 6, "binary expression");
    expression.kind = ABI_EXPRESSION_BINARY;
    expression.op = words[3];
    expression.expression_refs.push_back(words[4]);
    expression.expression_refs.push_back(words[5]);
    return record;
  }
  if(form == "conditional") {
    require_words(words, 6, "conditional expression");
    require_end(words, 6, "conditional expression");
    expression.kind = ABI_EXPRESSION_CONDITIONAL;
    for(std::size_t i = 3; i < 6; ++i) {
      expression.expression_refs.push_back(words[i]);
    }
    return record;
  }
  if(form == "pack") {
    require_words(words, 4, "pack expression");
    require_end(words, 4, "pack expression");
    expression.kind = ABI_EXPRESSION_PACK_EXPANSION;
    expression.expression_refs.push_back(words[3]);
    return record;
  }
  if(form == "call") {
    require_words(words, 4, "call expression");
    expression.kind = ABI_EXPRESSION_CALL;
    for(std::size_t i = 3; i < words.size(); ++i) {
      expression.expression_refs.push_back(words[i]);
    }
    return record;
  }
  if(form == "conversion" || form == "cast") {
    require_words(words, 6, "conversion expression");
    expression.kind = form == "conversion" ? ABI_EXPRESSION_CONVERSION :
      ABI_EXPRESSION_CAST;
    expression.op = words[3];
    const ParsedType converted = parse_type_at(words, 4);
    require_end(words, converted.next + 1, "conversion expression");
    expression.type = converted.type;
    expression.expression_refs.push_back(words[converted.next]);
    return record;
  }
  if(form == "template-id") {
    require_words(words, 4, "template-id expression");
    expression.kind = ABI_EXPRESSION_TEMPLATE_ID;
    expression.text = words[3];
    for(std::size_t i = 4; i < words.size(); ++i) {
      expression.argument_refs.push_back(words[i]);
    }
    return record;
  }
  if(form == "type-trait") {
    require_words(words, 5, "type-trait expression");
    expression.kind = ABI_EXPRESSION_TYPE_TRAIT;
    expression.text = words[3];
    std::size_t next = 4;
    while(next < words.size()) {
      const ParsedType operand = parse_type_at(words, next);
      expression.type_arguments.push_back(operand.type);
      next = operand.next;
    }
    return record;
  }
  if(form == "sizeof-type") {
    require_words(words, 4, "sizeof expression");
    const ParsedType operand = parse_type_at(words, 3);
    require_end(words, operand.next, "sizeof expression");
    expression.kind = ABI_EXPRESSION_SIZEOF_TYPE;
    expression.type = operand.type;
    return record;
  }
  if(form == "member") {
    require_words(words, 6, "member expression");
    const ParsedType owner = parse_type_at(words, 3);
    require_words(words, owner.next + 2, "member expression");
    expression.kind = ABI_EXPRESSION_MEMBER;
    expression.type = owner.type;
    expression.close_member_owner = fact_flag(words[owner.next]);
    expression.text = words[owner.next + 1];
    for(std::size_t i = owner.next + 2; i < words.size(); ++i) {
      expression.argument_refs.push_back(words[i]);
    }
    return record;
  }
  if(form == "object-member") {
    require_words(words, 6, "object member expression");
    expression.kind = ABI_EXPRESSION_OBJECT_MEMBER;
    expression.op = words[3];
    expression.expression_refs.push_back(words[4]);
    expression.text = words[5];
    for(std::size_t i = 6; i < words.size(); ++i) {
      expression.argument_refs.push_back(words[i]);
    }
    return record;
  }
  if(form == "entity-reference" || form == "entity") {
    require_end(words, 4, "entity expression");
    expression.kind = form == "entity-reference" ?
      ABI_EXPRESSION_EXTERNAL_ENTITY : ABI_EXPRESSION_ENTITY;
    expression.entity_ref = words[3];
    return record;
  }
  throw std::logic_error("unknown ABI expression form '" + form + "'");
}

AbiDefinitionRecord parse_context_definition(const Words & words)
{
  require_words(words, 4, "context definition");
  AbiDefinitionRecord record;
  record.kind = ABI_DEFINITION_CONTEXT;
  record.id = words[1];
  if(words[2] == "raw") {
    record.context.kind = ABI_CONTEXT_RAW;
    record.context.fragment = words[3];
    require_end(words, 4, "raw context definition");
    return record;
  }
  if(words[2] == "function") {
    record.context.kind = ABI_CONTEXT_FUNCTION;
    record.context.function = parse_function_target(words, 2);
    return record;
  }
  throw std::logic_error("unknown ABI context form '" + words[2] + "'");
}

AbiDefinitionRecord parse_entity_definition(const Words & words)
{
  require_words(words, 4, "entity definition");
  AbiDefinitionRecord record;
  record.kind = ABI_DEFINITION_ENTITY;
  record.id = words[1];
  const std::string & form = words[2];
  if(form == "variable" || form == "internal-variable") {
    require_end(words, 4, "variable entity definition");
    record.entity.kind = form == "variable" ? ABI_ENTITY_FACT_VARIABLE :
      ABI_ENTITY_FACT_VARIABLE;
    record.entity.internal_linkage = form == "internal-variable";
    record.entity.qualified_name = words[3];
    return record;
  }
  if(form == "symbol") {
    require_end(words, 4, "symbol entity definition");
    record.entity.kind = ABI_ENTITY_FACT_SYMBOL;
    record.entity.qualified_name = words[3];
    return record;
  }
  if(form == "function") {
    record.entity.kind = ABI_ENTITY_FACT_FUNCTION;
    record.entity.function = parse_function_target(words, 2);
    return record;
  }
  throw std::logic_error("unknown ABI entity form '" + form + "'");
}

AbiFactRecord parse_definition_words(const Words & words)
{
  AbiFactRecord record;
  record.kind = ABI_FACT_RECORD_DEFINITION;
  if(words[0] == "let-type") {
    record.definition = parse_type_definition(words);
  } else if(words[0] == "let-arg") {
    record.definition = parse_argument_definition(words);
  } else if(words[0] == "let-expr") {
    record.definition = parse_expression_definition(words);
  } else if(words[0] == "let-context") {
    record.definition = parse_context_definition(words);
  } else {
    record.definition = parse_entity_definition(words);
  }
  return record;
}

AbiFactRecord parse_target_words(const Words & words)
{
  require_words(words, 2, "target");
  AbiFactRecord record;
  record.kind = ABI_FACT_RECORD_TARGET;
  AbiTargetRecord & target = record.target;
  const std::string & form = words[0];
  if(form == "type" || form == "typeinfo" || form == "vtable" || form == "vtt") {
    const ParsedType parsed = parse_type_at(words, 1);
    require_end(words, parsed.next, "type target");
    target.kind = form == "type" ? ABI_TARGET_FACT_TYPE :
      form == "typeinfo" ? ABI_TARGET_FACT_TYPEINFO :
      form == "vtable" ? ABI_TARGET_FACT_VTABLE : ABI_TARGET_FACT_VTT;
    target.type = parsed.type;
    return record;
  }
  if(form == "variable") {
    require_end(words, 2, "variable target");
    target.kind = ABI_TARGET_FACT_VARIABLE;
    target.qualified_name = words[1];
    return record;
  }
  if(form == "c-function") {
    target.kind = ABI_TARGET_FACT_FUNCTION;
    target.c_linkage = true;
    target.function.kind = ABI_FUNCTION_TARGET_PATH;
    target.function.qualified_name = words[1];
    std::size_t next = 2;
    while(next < words.size()) {
      target.function.path_operands.push_back(path_operand(words, &next));
    }
    return record;
  }
  if(form == "function") {
    target.kind = ABI_TARGET_FACT_FUNCTION;
    target.function = parse_function_target(words, 0);
    return record;
  }
  if(form == "construction-vtable") {
    require_words(words, 4, "construction-vtable target");
    const ParsedType complete = parse_type_at(words, 1);
    if(complete.next >= words.size()) {
      throw std::logic_error("construction-vtable needs an offset and base");
    }
    target.base_offset = decimal_index(words[complete.next]);
    const ParsedType base = parse_type_at(words, complete.next + 1);
    require_end(words, base.next, "construction-vtable target");
    target.kind = ABI_TARGET_FACT_CONSTRUCTION_VTABLE;
    target.type = complete.type;
    target.base_type = base.type;
    return record;
  }
  if(form == "tls-wrapper") {
    require_words(words, 3, "TLS wrapper target");
    require_end(words, 3, "TLS wrapper target");
    if(words[1] != "variable") {
      throw std::logic_error("TLS wrapper target needs a variable");
    }
    target.kind = ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER;
    target.qualified_name = words[2];
    return record;
  }
  if(form == "thunk" || form == "virtual-base-thunk") {
    require_words(words, form == "thunk" ? 4 : 4, "thunk target");
    target.kind = form == "thunk" ? ABI_TARGET_FACT_THUNK :
      ABI_TARGET_FACT_VIRTUAL_BASE_THUNK;
    target.this_adjust = signed_value(words[1]);
    std::size_t function_pos = 2;
    if(form == "virtual-base-thunk") {
      target.vcall_offset = target.this_adjust;
      target.this_adjust = 0;
    } else if(words[function_pos] == "virtual-result") {
      require_words(words, function_pos + 4, "virtual-result thunk");
      target.has_result_adjust = true;
      target.result_adjust_virtual = true;
      target.result_adjust = signed_value(words[function_pos + 1]);
      target.result_vcall_offset = signed_value(words[function_pos + 2]);
      function_pos += 3;
    } else if(words[function_pos] != "function") {
      target.has_result_adjust = true;
      target.result_adjust = signed_value(words[function_pos]);
      ++function_pos;
      if(words[function_pos] == "virtual-result") {
        require_words(words, function_pos + 4, "virtual-result thunk");
        target.result_adjust_virtual = true;
        target.result_adjust = signed_value(words[function_pos + 1]);
        target.result_vcall_offset = signed_value(words[function_pos + 2]);
        function_pos += 3;
      }
    }
    if(function_pos >= words.size() || words[function_pos] != "function") {
      throw std::logic_error("thunk target needs a function");
    }
    target.function = parse_function_target(words, function_pos);
    return record;
  }
  throw std::logic_error("unknown ABI target '" + form + "'");
}

AbiFunctionQualifier parse_qualifier(const std::string & word)
{
  if(word == "const") {
    return ABI_FUNCTION_QUALIFIER_CONST;
  }
  if(word == "volatile") {
    return ABI_FUNCTION_QUALIFIER_VOLATILE;
  }
  if(word == "lvalue-ref" || word == "&") {
    return ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE;
  }
  if(word == "rvalue-ref" || word == "&&") {
    return ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE;
  }
  throw std::logic_error("unknown function qualifier '" + word + "'");
}

AbiFactRecord parse_function_record_words(const Words & words)
{
  require_words(words, 1, "function record");
  AbiFactRecord record;
  record.kind = ABI_FACT_RECORD_FUNCTION;
  AbiFunctionRecord & function = record.function;
  const std::string & form = words[0];
  if(form == "name-source") {
    require_words(words, 2, "name-source record");
    if(words.size() > 3) {
      throw std::logic_error("extra words in name-source record");
    }
    function.kind = ABI_FUNCTION_RECORD_NAME_SOURCE;
    function.source_name = words[1];
    function.substitution = words.size() == 3 ? words[2] : "";
    return record;
  }
  if(form == "name-std") {
    if(words.size() > 2) {
      throw std::logic_error("extra words in name-std record");
    }
    function.kind = ABI_FUNCTION_RECORD_NAME_STD;
    function.standard_substitution = words.size() == 2 ? words[1] : "";
    return record;
  }
  if(form == "name-template") {
    require_words(words, 6, "name-template record");
    function.kind = ABI_FUNCTION_RECORD_NAME_TEMPLATE;
    function.name = words[1];
    function.substitution = words[2];
    function.complete_substitution = words[3];
    function.standard_substitution = words[4];
    function.standard_substitution_includes_arguments = fact_flag(words[5]);
    for(std::size_t i = 6; i < words.size(); ++i) {
      function.argument_refs.push_back(words[i]);
    }
    return record;
  }
  if(form == "function-template-arg" || form == "function-template-prefix") {
    require_words(words, 2, "function template record");
    require_end(words, 2, "function template record");
    function.kind = form == "function-template-arg" ?
      ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT :
      ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX;
    function.substitution = words[1];
    function.argument_refs.push_back(words[1]);
    return record;
  }
  if(form == "local-context" || form == "lambda-context" ||
     form == "namespace-lambda-context") {
    require_words(words, 2, "local context record");
    function.kind = form == "local-context" ? ABI_FUNCTION_RECORD_LOCAL_CONTEXT :
      form == "lambda-context" ? ABI_FUNCTION_RECORD_LAMBDA_CONTEXT :
      ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT;
    if(form == "namespace-lambda-context") {
      function.source_name = words[1];
      for(std::size_t i = 2; i < words.size(); ++i) {
        function.namespace_qualifiers.push_back(words[i]);
      }
      return record;
    }
    require_words(words, 4, "local context record");
    function.context_ref = words[1];
    function.source_name = words[2];
    function.discriminator = words[3];
    std::size_t next = 4;
    while(next < words.size()) {
      const ParsedType signature = parse_type_at(words, next);
      function.types.push_back(signature.type);
      next = signature.next;
    }
    return record;
  }
  if(form == "terminal-source" || form == "terminal" || form == "operator-terminal") {
    require_words(words, 2, "terminal record");
    function.kind = form == "terminal-source" ? ABI_FUNCTION_RECORD_TERMINAL_SOURCE :
      form == "terminal" ? ABI_FUNCTION_RECORD_TERMINAL :
      ABI_FUNCTION_RECORD_OPERATOR_TERMINAL;
    function.terminal = words[1];
    if(form == "operator-terminal" && words[1] == "literal") {
      require_end(words, 3, "literal operator record");
      function.literal_suffix = words[2];
    } else {
      require_end(words, 2, "terminal record");
    }
    return record;
  }
  if(form == "variadic") {
    require_end(words, 1, "variadic record");
    function.kind = ABI_FUNCTION_RECORD_VARIADIC;
    return record;
  }
  if(form == "abi-tag") {
    require_words(words, 2, "ABI tag record");
    require_end(words, 2, "ABI tag record");
    function.kind = ABI_FUNCTION_RECORD_ABI_TAG;
    function.name = words[1];
    return record;
  }
  if(form == "qualifier" || form == "function-qualifier") {
    require_words(words, 2, "qualifier record");
    function.kind = ABI_FUNCTION_RECORD_QUALIFIER;
    for(std::size_t i = 1; i < words.size(); ++i) {
      function.qualifiers.push_back(parse_qualifier(words[i]));
    }
    return record;
  }
  if(form == "conversion-terminal") {
    require_words(words, 2, "conversion terminal record");
    const ParsedType converted = parse_type_at(words, 1);
    require_end(words, converted.next, "conversion terminal record");
    function.kind = ABI_FUNCTION_RECORD_CONVERSION_TERMINAL;
    function.type = converted.type;
    return record;
  }
  if(form == "param" || form == "result") {
    require_words(words, 2, "function type record");
    const ParsedType type = parse_type_at(words, 1);
    require_end(words, type.next, "function type record");
    function.kind = form == "param" ? ABI_FUNCTION_RECORD_PARAMETER :
      ABI_FUNCTION_RECORD_RESULT;
    function.type = type.type;
    return record;
  }
  throw std::logic_error("unknown function record '" + form + "'");
}

bool is_target_word(const std::string & word)
{
  return word == "type" || word == "function" || word == "variable" ||
    word == "typeinfo" || word == "vtable" || word == "vtt" ||
    word == "construction-vtable" || word == "tls-wrapper" ||
    word == "thunk" || word == "virtual-base-thunk" || word == "c-function";
}

}  // namespace

AbiFactRecord parse_fact_record_words(const std::vector<std::string> & words)
{
  if(words.empty()) {
    throw std::logic_error("empty ABI fact record");
  }
  if(words[0] == "let-type" || words[0] == "let-arg" ||
     words[0] == "let-expr" || words[0] == "let-context" ||
     words[0] == "let-entity") {
    return parse_definition_words(words);
  }
  if(is_target_word(words[0])) {
    return parse_target_words(words);
  }
  return parse_function_record_words(words);
}

namespace {

void validate_case(const AbiFactCase & fact_case)
{
  std::map<std::string, bool> ids;
  std::size_t target_count = 0;
  for(std::size_t i = 0; i < fact_case.records.size(); ++i) {
    const AbiFactRecord & record = fact_case.records[i];
    if(record.kind == ABI_FACT_RECORD_DEFINITION) {
      const std::string & id = record.definition.id;
      if(!ids.insert(std::make_pair(id, true)).second) {
        throw std::logic_error("duplicate ABI definition '" + id + "'");
      }
    } else if(record.kind == ABI_FACT_RECORD_TARGET) {
      ++target_count;
    }
  }
  if(target_count != 1) {
    throw std::logic_error("ABI case must contain exactly one target");
  }
}

void finish_case(AbiFactFile * file, AbiFactCase * current, bool * active)
{
  if(!*active) {
    return;
  }
  validate_case(*current);
  file->cases.push_back(std::move(*current));
  *active = false;
}

}  // namespace

AbiFactFile parse_fact_text(const std::string & text)
{
  AbiFactFile file;
  AbiFactCase current;
  bool active = false;
  std::istringstream input(text);
  std::string line;
  while(std::getline(input, line)) {
    const Words words = split_words(line);
    if(words.empty()) {
      continue;
    }
    if(words[0] == "case") {
      require_words(words, 2, "case header");
      require_end(words, 2, "case header");
      finish_case(&file, &current, &active);
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
  finish_case(&file, &current, &active);
  if(file.cases.empty()) {
    throw std::logic_error("ABI fact file contains no cases");
  }
  return file;
}

namespace {

std::string decimal_word(std::size_t value)
{
  std::ostringstream output;
  output << value;
  return output.str();
}

std::string signed_word(long long value)
{
  std::ostringstream output;
  output << value;
  return output.str();
}

std::string flag_word(bool value)
{
  return value ? "yes" : "no";
}

void append_word(std::string * line, const std::string & word)
{
  if(!line->empty()) {
    *line += " ";
  }
  *line += word;
}

std::string serialize_type(const AbiType & type);

std::string serialize_type_core(const AbiType & type)
{
  if(type.tagged) {
    AbiType base = type;
    base.tagged = false;
    base.abi_tags.clear();
    std::string line = "tagged " + serialize_type(base);
    for(std::size_t i = 0; i < type.abi_tags.size(); ++i) {
      append_word(&line, type.abi_tags[i]);
    }
    return line;
  }
  switch(type.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
    return type.name;
  case ABI_TYPE_NAMED:
    return "named:" + type.name;
  case ABI_TYPE_BUILTIN:
    return type.name;
  case ABI_TYPE_TEMPLATE_PARAMETER:
    return std::string(type.substitutable ? "template-param-subst " :
      "template-param ") + decimal_word(type.index);
  case ABI_TYPE_POINTER:
    return "ptr:" + serialize_type(type.types.at(0));
  case ABI_TYPE_LVALUE_REFERENCE:
    return "ref:" + serialize_type(type.types.at(0));
  case ABI_TYPE_RVALUE_REFERENCE:
    return "rref:" + serialize_type(type.types.at(0));
  case ABI_TYPE_CV:
    return serialize_type(type.types.at(0));
  case ABI_TYPE_PACK_EXPANSION:
    return "pack " + serialize_type(type.types.at(0));
  case ABI_TYPE_ARRAY:
    return "array:" + type.array_bound.value + ":" +
      serialize_type(type.types.at(0));
  case ABI_TYPE_VENDOR_QUALIFIED:
    return "vendor " + type.name + " " + serialize_type(type.types.at(0));
  case ABI_TYPE_BUILTIN_TRANSFORM:
    return "builtin-transform " + type.name + " " +
      serialize_type(type.types.at(0));
  case ABI_TYPE_FUNCTION: {
    std::string line = type.variadic ? "function-type-variadic" : "function-type";
    for(std::size_t i = 0; i < type.types.size(); ++i) {
      append_word(&line, serialize_type(type.types[i]));
    }
    return line;
  }
  case ABI_TYPE_MEMBER_POINTER:
    return "member-pointer " + serialize_type(type.types.at(0)) + " " +
      serialize_type(type.types.at(1));
  case ABI_TYPE_TEMPLATE_SPECIALIZATION: {
    std::string line = "template " + type.name;
    for(std::size_t i = 0; i < type.argument_refs.size(); ++i) {
      append_word(&line, type.argument_refs[i]);
    }
    return line;
  }
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION: {
    std::string line = "template-param-template " + decimal_word(type.index);
    for(std::size_t i = 0; i < type.argument_refs.size(); ++i) {
      append_word(&line, type.argument_refs[i]);
    }
    return line;
  }
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION: {
    std::string line = "std-template " + type.standard_substitution + " " +
      flag_word(type.standard_substitution_includes_arguments) + " " + type.name;
    for(std::size_t i = 0; i < type.argument_refs.size(); ++i) {
      append_word(&line, type.argument_refs[i]);
    }
    return line;
  }
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION: {
    std::string line = type.kind == ABI_TYPE_MEMBER ? "member " : "member-template ";
    append_word(&line, serialize_type(type.types.at(0)));
    append_word(&line, type.name);
    for(std::size_t i = 0; i < type.argument_refs.size(); ++i) {
      append_word(&line, type.argument_refs[i]);
    }
    return line;
  }
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    return "decltype " + type.expression_ref;
  case ABI_TYPE_LAMBDA_CLOSURE: {
    std::string line = "lambda-closure " + type.context_ref + " " +
      type.discriminator;
    for(std::size_t i = 0; i < type.types.size(); ++i) {
      append_word(&line, serialize_type(type.types[i]));
    }
    return line;
  }
  case ABI_TYPE_LOCAL_TYPE:
    return "local-type " + type.context_ref + " " + type.name + " " +
      type.discriminator;
  case ABI_TYPE_NAMESPACE_LAMBDA: {
    std::string line = "namespace-lambda " + type.name;
    for(std::size_t i = 0; i < type.namespace_qualifiers.size(); ++i) {
      append_word(&line, type.namespace_qualifiers[i]);
    }
    return line;
  }
  }
  throw std::logic_error("cannot serialize unknown ABI type");
}

std::string serialize_type(const AbiType & type)
{
  std::string line = serialize_type_core(type);
  if(type.is_const && type.kind != ABI_TYPE_CV) {
    line = "const " + line;
  }
  if(type.is_volatile && type.kind != ABI_TYPE_CV) {
    line = "volatile " + line;
  }
  return line;
}

std::string serialize_path_operand(const AbiFunctionPathOperand & operand)
{
  if(operand.kind == ABI_FUNCTION_PATH_VARIADIC) {
    return "variadic";
  }
  if(operand.kind == ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT) {
    return operand.argument_ref;
  }
  return serialize_type(operand.type);
}

std::string serialize_function_target(const AbiFunctionTarget & target)
{
  if(target.kind == ABI_FUNCTION_TARGET_ENCODING) {
    return "function encoding";
  }
  if(target.kind == ABI_FUNCTION_TARGET_LOCAL) {
    return "function local " + target.context_ref + " " + target.source_name +
      " " + target.terminal + " " + target.discriminator;
  }
  if(target.kind == ABI_FUNCTION_TARGET_LAMBDA) {
    return "function lambda " + target.context_ref + " " + target.discriminator +
      " " + target.terminal;
  }
  if(target.kind == ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA) {
    std::string line = "function namespace-lambda " + target.source_name + " " +
      target.terminal;
    for(std::size_t i = 0; i < target.namespace_qualifiers.size(); ++i) {
      append_word(&line, target.namespace_qualifiers[i]);
    }
    return line;
  }
  std::string line = "function path " + target.qualified_name;
  for(std::size_t i = 0; i < target.path_operands.size(); ++i) {
    append_word(&line, serialize_path_operand(target.path_operands[i]));
  }
  return line;
}

std::string serialize_expression(const AbiDependentExpression & expression)
{
  std::string line;
  switch(expression.kind) {
  case ABI_EXPRESSION_TEMPLATE_PARAMETER:
    return "template-param " + decimal_word(expression.index);
  case ABI_EXPRESSION_FUNCTION_PARAMETER:
    return "function-param " + decimal_word(expression.index);
  case ABI_EXPRESSION_LITERAL:
    return "literal " + expression.text;
  case ABI_EXPRESSION_INTEGRAL_VALUE:
    return "integral-value " + signed_word(expression.value);
  case ABI_EXPRESSION_UNARY:
    return "unary " + expression.op + " " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_BINARY:
    return "binary " + expression.op + " " + expression.expression_refs.at(0) +
      " " + expression.expression_refs.at(1);
  case ABI_EXPRESSION_CONDITIONAL:
    return "conditional " + expression.expression_refs.at(0) + " " +
      expression.expression_refs.at(1) + " " + expression.expression_refs.at(2);
  case ABI_EXPRESSION_PACK_EXPANSION:
    return "pack " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_CALL:
    line = "call";
    break;
  case ABI_EXPRESSION_CONVERSION:
    return "conversion " + expression.op + " " + serialize_type(expression.type) +
      " " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_CAST:
    return "cast " + expression.op + " " + serialize_type(expression.type) +
      " " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_TEMPLATE_ID:
    line = "template-id " + expression.text;
    break;
  case ABI_EXPRESSION_TYPE_TRAIT:
    line = "type-trait " + expression.text;
    for(std::size_t i = 0; i < expression.type_arguments.size(); ++i) {
      append_word(&line, serialize_type(expression.type_arguments[i]));
    }
    return line;
  case ABI_EXPRESSION_SIZEOF_TYPE:
    return "sizeof-type " + serialize_type(expression.type);
  case ABI_EXPRESSION_MEMBER:
    line = "member " + serialize_type(expression.type) + " " +
      flag_word(expression.close_member_owner) + " " + expression.text;
    break;
  case ABI_EXPRESSION_OBJECT_MEMBER:
    line = "object-member " + expression.op + " " +
      expression.expression_refs.at(0) + " " + expression.text;
    break;
  case ABI_EXPRESSION_EXTERNAL_ENTITY:
    return "entity-reference " + expression.entity_ref;
  case ABI_EXPRESSION_ENTITY:
    return "entity " + expression.entity_ref;
  }
  for(std::size_t i = 0; i < expression.expression_refs.size(); ++i) {
    if((expression.kind == ABI_EXPRESSION_CALL && i == 0) ||
       expression.kind == ABI_EXPRESSION_TEMPLATE_ID) {
      append_word(&line, expression.expression_refs[i]);
    }
  }
  for(std::size_t i = 0; i < expression.argument_refs.size(); ++i) {
    append_word(&line, expression.argument_refs[i]);
  }
  return line;
}

std::string serialize_definition(const AbiDefinitionRecord & definition)
{
  if(definition.kind == ABI_DEFINITION_TYPE) {
    return "let-type " + definition.id + " " + serialize_type(definition.type);
  }
  if(definition.kind == ABI_DEFINITION_TEMPLATE_ARGUMENT) {
    const AbiTemplateArgument & argument = definition.template_argument;
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_TYPE) {
      return "let-arg " + definition.id + " type " + serialize_type(argument.type);
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_VALUE) {
      return "let-arg " + definition.id + " value " +
        serialize_type(argument.value_type) + " " + signed_word(argument.value);
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE) {
      return "let-arg " + definition.id + " dependent-value " +
        argument.expression_ref + " " + serialize_type(argument.value_type) +
        " " + signed_word(argument.value);
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_EXPRESSION) {
      return "let-arg " + definition.id + " expression " + argument.expression_ref;
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE) {
      return "let-arg " + definition.id + " template-param-template " +
        decimal_word(argument.index);
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY) {
      return "let-arg " + definition.id + " template-entity " + argument.name;
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY) {
      return "let-arg " + definition.id + " member-template-entity " +
        serialize_type(argument.type) + " " + argument.name + " " +
        argument.substitution;
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_ENTITY) {
      return "let-arg " + definition.id + " entity-address " + argument.entity_ref;
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE) {
      return "let-arg " + definition.id + " untyped-value " +
        signed_word(argument.value);
    }
    if(argument.kind == ABI_TEMPLATE_ARGUMENT_PACK) {
      std::string line = "let-arg " + definition.id + " pack";
      for(std::size_t i = 0; i < argument.argument_refs.size(); ++i) {
        append_word(&line, argument.argument_refs[i]);
      }
      return line;
    }
    std::string line = "let-arg " + definition.id +
      " member-external-address " + argument.symbol + " " +
      serialize_type(argument.owner_type) + " " + argument.name + " " +
      flag_word(argument.member_is_function) + " " +
      flag_word(argument.member_function_const) + " " +
      flag_word(argument.member_function_volatile) + " " +
      flag_word(argument.member_function_lvalue_ref) + " " +
      flag_word(argument.member_function_rvalue_ref) + " " +
      flag_word(argument.member_function_variadic);
    for(std::size_t i = 0; i < argument.parameter_types.size(); ++i) {
      append_word(&line, serialize_type(argument.parameter_types[i]));
    }
    return line;
  }
  if(definition.kind == ABI_DEFINITION_EXPRESSION) {
    return "let-expr " + definition.id + " " +
      serialize_expression(definition.expression);
  }
  if(definition.kind == ABI_DEFINITION_CONTEXT) {
    if(definition.context.kind == ABI_CONTEXT_RAW) {
      return "let-context " + definition.id + " raw " +
        definition.context.fragment;
    }
    return "let-context " + definition.id + " " +
      serialize_function_target(definition.context.function);
  }
  const AbiEntityFact & entity = definition.entity;
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return "let-entity " + definition.id + " symbol " + entity.qualified_name;
  }
  if(entity.kind == ABI_ENTITY_FACT_FUNCTION) {
    return "let-entity " + definition.id + " " +
      serialize_function_target(entity.function);
  }
  return "let-entity " + definition.id + " " +
    (entity.internal_linkage ? "internal-variable " : "variable ") +
    entity.qualified_name;
}

std::string serialize_function_record(const AbiFunctionRecord & function)
{
  if(function.kind == ABI_FUNCTION_RECORD_NAME_SOURCE) {
    std::string line = "name-source " + function.source_name;
    if(!function.substitution.empty()) {
      append_word(&line, function.substitution);
    }
    return line;
  }
  if(function.kind == ABI_FUNCTION_RECORD_NAME_STD) {
    return function.standard_substitution.empty() ? "name-std" :
      "name-std " + function.standard_substitution;
  }
  if(function.kind == ABI_FUNCTION_RECORD_NAME_TEMPLATE) {
    std::string line = "name-template " + function.name + " " +
      function.substitution + " " + function.complete_substitution + " " +
      (function.standard_substitution.empty() ? "-" : function.standard_substitution) +
      " " + flag_word(function.standard_substitution_includes_arguments);
    for(std::size_t i = 0; i < function.argument_refs.size(); ++i) {
      append_word(&line, function.argument_refs[i]);
    }
    return line;
  }
  if(function.kind == ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT) {
    return "function-template-arg " + function.substitution;
  }
  if(function.kind == ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX) {
    return "function-template-prefix " + function.substitution;
  }
  if(function.kind == ABI_FUNCTION_RECORD_LOCAL_CONTEXT ||
     function.kind == ABI_FUNCTION_RECORD_LAMBDA_CONTEXT) {
    const std::string prefix = function.kind == ABI_FUNCTION_RECORD_LOCAL_CONTEXT ?
      "local-context " : "lambda-context ";
    std::string line = prefix + function.context_ref + " " + function.source_name +
      " " + function.discriminator;
    for(std::size_t i = 0; i < function.types.size(); ++i) {
      append_word(&line, serialize_type(function.types[i]));
    }
    return line;
  }
  if(function.kind == ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT) {
    std::string line = "namespace-lambda-context " + function.source_name;
    for(std::size_t i = 0; i < function.namespace_qualifiers.size(); ++i) {
      append_word(&line, function.namespace_qualifiers[i]);
    }
    return line;
  }
  if(function.kind == ABI_FUNCTION_RECORD_VARIADIC) {
    return "variadic";
  }
  if(function.kind == ABI_FUNCTION_RECORD_ABI_TAG) {
    return "abi-tag " + function.name;
  }
  if(function.kind == ABI_FUNCTION_RECORD_QUALIFIER) {
    std::string line = "qualifier";
    for(std::size_t i = 0; i < function.qualifiers.size(); ++i) {
      const AbiFunctionQualifier qualifier = function.qualifiers[i];
      append_word(&line, qualifier == ABI_FUNCTION_QUALIFIER_CONST ? "const" :
        qualifier == ABI_FUNCTION_QUALIFIER_VOLATILE ? "volatile" :
        qualifier == ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE ? "lvalue-ref" :
        "rvalue-ref");
    }
    return line;
  }
  if(function.kind == ABI_FUNCTION_RECORD_CONVERSION_TERMINAL) {
    return "conversion-terminal " + serialize_type(function.type);
  }
  if(function.kind == ABI_FUNCTION_RECORD_PARAMETER) {
    return "param " + serialize_type(function.type);
  }
  if(function.kind == ABI_FUNCTION_RECORD_RESULT) {
    return "result " + serialize_type(function.type);
  }
  if(function.kind == ABI_FUNCTION_RECORD_TERMINAL_SOURCE) {
    return "terminal-source " + function.terminal;
  }
  if(function.kind == ABI_FUNCTION_RECORD_OPERATOR_TERMINAL &&
     function.terminal == "literal") {
    return "operator-terminal literal " + function.literal_suffix;
  }
  if(function.kind == ABI_FUNCTION_RECORD_OPERATOR_TERMINAL) {
    return "operator-terminal " + function.terminal;
  }
  return "terminal " + function.terminal;
}

std::string serialize_target(const AbiTargetRecord & target)
{
  if(target.kind == ABI_TARGET_FACT_TYPE) {
    return "type " + serialize_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VARIABLE) {
    return "variable " + target.qualified_name;
  }
  if(target.kind == ABI_TARGET_FACT_TYPEINFO) {
    return "typeinfo " + serialize_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VTABLE) {
    return "vtable " + serialize_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_VTT) {
    return "vtt " + serialize_type(target.type);
  }
  if(target.kind == ABI_TARGET_FACT_CONSTRUCTION_VTABLE) {
    return "construction-vtable " + serialize_type(target.type) + " " +
      decimal_word(target.base_offset) + " " + serialize_type(target.base_type);
  }
  if(target.kind == ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER) {
    return "tls-wrapper variable " + target.qualified_name;
  }
  if(target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
    return "virtual-base-thunk " + signed_word(target.vcall_offset) + " " +
      serialize_function_target(target.function);
  }
  if(target.kind == ABI_TARGET_FACT_THUNK) {
    std::string line = "thunk " + signed_word(target.this_adjust);
    if(target.has_result_adjust) {
      append_word(&line, target.result_adjust_virtual ? "virtual-result" :
        signed_word(target.result_adjust));
      if(target.result_adjust_virtual) {
        append_word(&line, signed_word(target.result_adjust));
        append_word(&line, signed_word(target.result_vcall_offset));
      }
    }
    append_word(&line, serialize_function_target(target.function));
    return line;
  }
  return target.c_linkage ? "c-function " + target.function.qualified_name :
    serialize_function_target(target.function);
}

std::string serialize_record(const AbiFactRecord & record)
{
  if(record.kind == ABI_FACT_RECORD_DEFINITION) {
    return serialize_definition(record.definition);
  }
  if(record.kind == ABI_FACT_RECORD_TARGET) {
    return serialize_target(record.target);
  }
  return serialize_function_record(record.function);
}

}  // namespace

std::string serialize_fact_file(const AbiFactFile & file)
{
  std::string output;
  for(std::size_t i = 0; i < file.cases.size(); ++i) {
    const AbiFactCase & fact_case = file.cases[i];
    if(!fact_case.label.empty()) {
      output += "case " + fact_case.label + "\n";
    }
    for(std::size_t j = 0; j < fact_case.records.size(); ++j) {
      output += serialize_record(fact_case.records[j]);
      output += "\n";
    }
    if(i + 1 < file.cases.size() && fact_case.records.empty()) {
      output += "\n";
    }
  }
  return output;
}

}  // namespace abi_mangle
