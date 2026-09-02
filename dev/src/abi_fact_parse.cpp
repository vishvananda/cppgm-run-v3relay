// Line-oriented ABI fact reader and serializer: the abimangle tool boundary.
// Every fact word is classified here, once; the encoder consumes only the
// typed model.

#include "abi_mangle.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace abi_mangle {
namespace {

typedef std::vector<std::string> Words;

Words split_words(const std::string & line)
{
  Words words;
  std::size_t i = 0;
  while(i < line.size()) {
    while(i < line.size() && (line[i] == ' ' || line[i] == '\t' ||
                              line[i] == '\r')) {
      ++i;
    }
    const std::size_t start = i;
    while(i < line.size() && line[i] != ' ' && line[i] != '\t' &&
          line[i] != '\r') {
      ++i;
    }
    if(i > start) {
      words.push_back(line.substr(start, i - start));
    }
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

void require_depth(std::size_t depth)
{
  if(depth > ABI_MAXIMUM_NESTING_DEPTH) {
    throw std::logic_error("ABI type nests too deeply");
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
  if((value == std::numeric_limits<long long>::min() ||
      value == std::numeric_limits<long long>::max()) &&
     word != "-9223372036854775808" && word != "9223372036854775807") {
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

bool is_dash(const std::string & word)
{
  return word.empty() || word == "-";
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

void wrap_type(AbiTypeKind kind, AbiType * type)
{
  AbiType wrapper;
  wrapper.kind = kind;
  wrapper.types.push_back(std::move(*type));
  *type = std::move(wrapper);
}

// Compact single-word type grammar: builtin | id | Q | named:Q | ptr:T |
// ref:T | rref:T | const:T | volatile:T | pack:T | array:N:T |
// memberptr:O:M.
void compact_type(const std::string & word, std::size_t depth, AbiType * out)
{
  require_depth(depth);
  if(lookup_builtin_type(word, &out->builtin)) {
    out->kind = ABI_TYPE_BUILTIN;
    return;
  }
  const std::size_t colon = word.find(':');
  if(colon == std::string::npos || (colon + 1 < word.size() &&
                                    word[colon + 1] == ':')) {
    out->kind = ABI_TYPE_NAME_OR_REFERENCE;
    out->name = word;
    return;
  }
  const std::string head = word.substr(0, colon);
  const std::string tail = word.substr(colon + 1);
  if(tail.empty()) {
    throw std::logic_error("empty compact ABI type operand");
  }
  if(head == "named") {
    out->kind = ABI_TYPE_NAMED;
    out->name = tail;
    return;
  }
  if(head == "ptr" || head == "ref" || head == "rref" || head == "pack") {
    compact_type(tail, depth + 1, out);
    wrap_type(head == "ptr" ? ABI_TYPE_POINTER :
              head == "ref" ? ABI_TYPE_LVALUE_REFERENCE :
              head == "rref" ? ABI_TYPE_RVALUE_REFERENCE :
              ABI_TYPE_PACK_EXPANSION, out);
    return;
  }
  if(head == "const" || head == "volatile") {
    compact_type(tail, depth + 1, out);
    out->is_const = out->is_const || head == "const";
    out->is_volatile = out->is_volatile || head == "volatile";
    return;
  }
  if(head == "array") {
    const std::size_t bound_end = tail.find(':');
    if(bound_end == std::string::npos) {
      throw std::logic_error("array type needs a bound");
    }
    AbiType element;
    compact_type(tail.substr(bound_end + 1), depth + 1, &element);
    out->kind = ABI_TYPE_ARRAY;
    out->array_bound.kind = ABI_ARRAY_BOUND_VALUE;
    out->array_bound.value = tail.substr(0, bound_end);
    decimal_index(out->array_bound.value);
    out->types.push_back(std::move(element));
    return;
  }
  if(head == "memberptr") {
    const std::size_t split = memberptr_separator(tail);
    AbiType owner;
    AbiType member;
    compact_type(tail.substr(0, split), depth + 1, &owner);
    compact_type(tail.substr(split + 1), depth + 1, &member);
    out->kind = ABI_TYPE_MEMBER_POINTER;
    out->types.push_back(std::move(owner));
    out->types.push_back(std::move(member));
    return;
  }
  throw std::logic_error("unknown compact ABI type '" + word + "'");
}

std::size_t parse_type_at(const Words & words, std::size_t pos,
                          std::size_t depth, AbiType * out);

std::size_t parse_type_sequence(const Words & words, std::size_t pos,
                                std::size_t depth, std::vector<AbiType> * out)
{
  while(pos < words.size()) {
    AbiType item;
    pos = parse_type_at(words, pos, depth, &item);
    out->push_back(std::move(item));
  }
  return pos;
}

std::size_t parse_argument_refs(const Words & words, std::size_t pos,
                                std::vector<std::string> * refs)
{
  for(; pos < words.size(); ++pos) {
    refs->push_back(words[pos]);
  }
  return pos;
}

bool accepts_abi_tags(AbiTypeKind kind)
{
  return kind == ABI_TYPE_NAME_OR_REFERENCE || kind == ABI_TYPE_NAMED ||
    kind == ABI_TYPE_TEMPLATE_SPECIALIZATION;
}

// Multiword type grammar.  Returns the index after the type.
std::size_t parse_type_at(const Words & words, std::size_t pos,
                          std::size_t depth, AbiType * out)
{
  require_depth(depth);
  require_words(words, pos + 1, "type");
  const std::string & head = words[pos];
  if(head == "vendor" || head == "builtin-transform") {
    require_words(words, pos + 3, "type");
    AbiType child;
    const std::size_t next = parse_type_at(words, pos + 2, depth + 1, &child);
    out->kind = head == "vendor" ? ABI_TYPE_VENDOR_QUALIFIED :
      ABI_TYPE_BUILTIN_TRANSFORM;
    out->name = words[pos + 1];
    out->types.push_back(std::move(child));
    return next;
  }
  if(head == "function-type" || head == "function-type-variadic") {
    require_words(words, pos + 2, "function type");
    out->kind = ABI_TYPE_FUNCTION;
    out->variadic = head == "function-type-variadic";
    return parse_type_sequence(words, pos + 1, depth + 1, &out->types);
  }
  if(head == "member-pointer") {
    require_words(words, pos + 3, "member-pointer type");
    AbiType owner;
    AbiType member;
    std::size_t next = parse_type_at(words, pos + 1, depth + 1, &owner);
    next = parse_type_at(words, next, depth + 1, &member);
    out->kind = ABI_TYPE_MEMBER_POINTER;
    out->types.push_back(std::move(owner));
    out->types.push_back(std::move(member));
    return next;
  }
  if(head == "template" || head == "std-template") {
    const bool standard = head == "std-template";
    std::size_t next = pos + 1;
    if(standard) {
      require_words(words, pos + 4, "template type");
      out->kind = ABI_TYPE_STD_TEMPLATE_SPECIALIZATION;
      out->standard_substitution = words[pos + 1];
      out->standard_substitution_includes_arguments = fact_flag(words[pos + 2]);
      next = pos + 3;
    } else {
      require_words(words, pos + 2, "template type");
      out->kind = ABI_TYPE_TEMPLATE_SPECIALIZATION;
    }
    out->name = words[next];
    return parse_argument_refs(words, next + 1, &out->argument_refs);
  }
  if(head == "template-param" || head == "template-param-subst") {
    require_words(words, pos + 2, "template parameter type");
    out->kind = ABI_TYPE_TEMPLATE_PARAMETER;
    out->index = decimal_index(words[pos + 1]);
    out->substitutable = head == "template-param-subst";
    return pos + 2;
  }
  if(head == "template-param-template") {
    require_words(words, pos + 2, "template parameter template type");
    out->kind = ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION;
    out->index = decimal_index(words[pos + 1]);
    return parse_argument_refs(words, pos + 2, &out->argument_refs);
  }
  if(head == "name") {
    require_words(words, pos + 2, "named type");
    out->kind = ABI_TYPE_NAMED;
    out->name = words[pos + 1];
    return pos + 2;
  }
  if(head == "decltype") {
    require_words(words, pos + 2, "decltype type");
    out->kind = ABI_TYPE_DECLTYPE_EXPRESSION;
    out->expression_ref = words[pos + 1];
    return pos + 2;
  }
  if(head == "member" || head == "member-template") {
    require_words(words, pos + 3, "member type");
    AbiType owner;
    std::size_t next = parse_type_at(words, pos + 1, depth + 1, &owner);
    require_words(words, next + 1, "member type");
    out->kind = head == "member" ? ABI_TYPE_MEMBER :
      ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION;
    out->types.push_back(std::move(owner));
    out->name = words[next++];
    if(head == "member-template") {
      next = parse_argument_refs(words, next, &out->argument_refs);
    }
    return next;
  }
  if(head == "lambda-closure") {
    require_words(words, pos + 3, "lambda closure type");
    out->kind = ABI_TYPE_LAMBDA_CLOSURE;
    out->context_ref = words[pos + 1];
    out->discriminator = words[pos + 2];
    return parse_type_sequence(words, pos + 3, depth + 1, &out->types);
  }
  if(head == "local-type") {
    require_words(words, pos + 4, "local type");
    out->kind = ABI_TYPE_LOCAL_TYPE;
    out->context_ref = words[pos + 1];
    out->name = words[pos + 2];
    out->discriminator = words[pos + 3];
    return pos + 4;
  }
  if(head == "namespace-lambda") {
    require_words(words, pos + 2, "namespace lambda type");
    out->kind = ABI_TYPE_NAMESPACE_LAMBDA;
    out->name = words[pos + 1];
    return parse_argument_refs(words, pos + 2, &out->namespace_qualifiers);
  }
  if(head == "tagged") {
    require_words(words, pos + 3, "tagged type");
    std::size_t next = parse_type_at(words, pos + 1, depth + 1, out);
    if(!accepts_abi_tags(out->kind)) {
      throw std::logic_error("ABI tags need a named or template type");
    }
    out->abi_tags.clear();
    return parse_argument_refs(words, next, &out->abi_tags);
  }
  if(head == "array") {
    require_words(words, pos + 3, "array type");
    AbiType element;
    const std::size_t next = parse_type_at(words, pos + 2, depth + 1, &element);
    out->kind = ABI_TYPE_ARRAY;
    out->array_bound.kind = ABI_ARRAY_BOUND_VALUE;
    out->array_bound.value = words[pos + 1];
    decimal_index(out->array_bound.value);
    out->types.push_back(std::move(element));
    return next;
  }
  if(head == "const" || head == "volatile" || head == "ptr" || head == "ref" ||
     head == "rref" || head == "pack") {
    require_words(words, pos + 2, "wrapped type");
    const std::size_t next = parse_type_at(words, pos + 1, depth + 1, out);
    if(head == "const") {
      out->is_const = true;
    } else if(head == "volatile") {
      out->is_volatile = true;
    } else {
      wrap_type(head == "ptr" ? ABI_TYPE_POINTER :
                head == "ref" ? ABI_TYPE_LVALUE_REFERENCE :
                head == "rref" ? ABI_TYPE_RVALUE_REFERENCE :
                ABI_TYPE_PACK_EXPANSION, out);
    }
    return next;
  }
  compact_type(head, depth + 1, out);
  return pos + 1;
}

// Terminal word of a compact local, lambda, or namespace-lambda target.
AbiTerminal compact_terminal(const std::string & word)
{
  AbiTerminal terminal;
  if(word == "operator-call" || word == "call") {
    terminal.kind = ABI_TERMINAL_OPERATOR;
    terminal.operator_kind = ABI_OPERATOR_CALL;
  } else if(lookup_special_function(word, &terminal.special_function)) {
    terminal.kind = ABI_TERMINAL_SPECIAL;
  } else {
    terminal.kind = ABI_TERMINAL_SOURCE;
    terminal.name = word;
  }
  return terminal;
}

// A bare operand word is provisionally a type reference; resolve_case turns
// it into a template argument when the case defines an argument by that id.
std::size_t parse_path_operand(const Words & words, std::size_t pos,
                               std::vector<AbiFunctionPathOperand> * operands)
{
  AbiFunctionPathOperand operand;
  if(words[pos] == "variadic") {
    operand.kind = ABI_FUNCTION_PATH_VARIADIC;
    operands->push_back(std::move(operand));
    return pos + 1;
  }
  operand.kind = ABI_FUNCTION_PATH_TYPE;
  const std::size_t next = parse_type_at(words, pos, 0, &operand.type);
  operands->push_back(std::move(operand));
  return next;
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
  if(form == "local") {
    require_words(words, pos + 6, "local function target");
    target.kind = ABI_FUNCTION_TARGET_LOCAL;
    target.context_ref = words[pos + 2];
    target.source_name = words[pos + 3];
    target.terminal = compact_terminal(words[pos + 4]);
    target.discriminator = words[pos + 5];
    parse_type_sequence(words, pos + 6, 0, &target.signature_parameter_types);
    return target;
  }
  if(form == "lambda") {
    require_words(words, pos + 5, "lambda function target");
    target.kind = ABI_FUNCTION_TARGET_LAMBDA;
    target.context_ref = words[pos + 2];
    target.discriminator = words[pos + 3];
    target.terminal = compact_terminal(words[pos + 4]);
    parse_type_sequence(words, pos + 5, 0, &target.signature_parameter_types);
    return target;
  }
  if(form == "namespace-lambda") {
    require_words(words, pos + 4, "namespace lambda target");
    target.kind = ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA;
    target.source_name = words[pos + 2];
    target.terminal = compact_terminal(words[pos + 3]);
    parse_argument_refs(words, pos + 4, &target.namespace_qualifiers);
    return target;
  }
  const std::size_t name_pos = form == "path" ? pos + 2 : pos + 1;
  require_words(words, name_pos + 1, "function target");
  target.kind = ABI_FUNCTION_TARGET_PATH;
  target.qualified_name = words[name_pos];
  std::size_t next = name_pos + 1;
  while(next < words.size()) {
    next = parse_path_operand(words, next, &target.path_operands);
  }
  return target;
}

// ---- definitions ----

void parse_type_definition(const Words & words, AbiFactCase * fact_case)
{
  require_words(words, 3, "type definition");
  AbiTypeDefinition definition;
  definition.id = words[1];
  require_end(words, parse_type_at(words, 2, 0, &definition.type),
              "type definition");
  fact_case->types.push_back(std::move(definition));
}

void parse_argument_definition(const Words & words, AbiFactCase * fact_case)
{
  require_words(words, 3, "argument definition");
  AbiArgumentDefinition definition;
  definition.id = words[1];
  AbiTemplateArgument & argument = definition.argument;
  const std::string & form = words[2];
  if(form == "type") {
    require_end(words, parse_type_at(words, 3, 0, &argument.type),
                "argument definition");
    argument.kind = ABI_TEMPLATE_ARGUMENT_TYPE;
  } else if(form == "value") {
    require_words(words, 5, "value argument");
    const std::size_t next = parse_type_at(words, 3, 0, &argument.value_type);
    require_end(words, next + 1, "value argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_VALUE;
    argument.value = signed_value(words[next]);
  } else if(form == "dependent-value") {
    require_words(words, 6, "dependent value argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE;
    argument.type.kind = ABI_TYPE_NAME_OR_REFERENCE;
    argument.type.name = words[3];
    const std::size_t next = parse_type_at(words, 4, 0, &argument.value_type);
    require_end(words, next + 1, "dependent value argument");
    argument.value = signed_value(words[next]);
  } else if(form == "expression") {
    require_words(words, 4, "expression argument");
    require_end(words, 4, "expression argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_EXPRESSION;
    argument.expression_ref = words[3];
  } else if(form == "template-param-template") {
    require_words(words, 4, "template parameter argument");
    require_end(words, 4, "template parameter argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE;
    argument.index = decimal_index(words[3]);
  } else if(form == "template-entity") {
    require_words(words, 4, "template entity argument");
    require_end(words, 4, "template entity argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY;
    argument.name = words[3];
  } else if(form == "member-template-entity") {
    require_words(words, 6, "member template entity argument");
    require_end(words, 6, "member template entity argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY;
    compact_type(words[3], 0, &argument.type);
    argument.name = words[4];
    argument.substitution = words[5];
  } else if(form == "entity-address") {
    require_words(words, 4, "entity argument");
    require_end(words, 4, "entity argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_ENTITY;
    argument.entity_ref = words[3];
    argument.address_of = true;
  } else if(form == "member-external-address") {
    require_words(words, 12, "external member argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY;
    argument.symbol = words[3];
    compact_type(words[4], 0, &argument.owner_type);
    argument.name = words[5];
    argument.member_is_function = fact_flag(words[6]);
    argument.member_function_const = fact_flag(words[7]);
    argument.member_function_volatile = fact_flag(words[8]);
    argument.member_function_lvalue_ref = fact_flag(words[9]);
    argument.member_function_rvalue_ref = fact_flag(words[10]);
    argument.member_function_variadic = fact_flag(words[11]);
    parse_type_sequence(words, 12, 0, &argument.parameter_types);
  } else if(form == "untyped-value") {
    require_words(words, 4, "untyped value argument");
    require_end(words, 4, "untyped value argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE;
    argument.value = signed_value(words[3]);
  } else if(form == "pack") {
    argument.kind = ABI_TEMPLATE_ARGUMENT_PACK;
    parse_argument_refs(words, 3, &argument.argument_refs);
  } else {
    throw std::logic_error("unknown ABI argument form '" + form + "'");
  }
  fact_case->arguments.push_back(std::move(definition));
}

void parse_expression_definition(const Words & words, AbiFactCase * fact_case)
{
  require_words(words, 4, "expression definition");
  AbiExpressionDefinition definition;
  definition.id = words[1];
  AbiDependentExpression & expression = definition.expression;
  const std::string & form = words[2];
  if(form == "template-param" || form == "function-param") {
    require_end(words, 4, "parameter expression");
    expression.kind = form == "template-param" ?
      ABI_EXPRESSION_TEMPLATE_PARAMETER : ABI_EXPRESSION_FUNCTION_PARAMETER;
    expression.index = decimal_index(words[3]);
  } else if(form == "literal" || form == "integral-value") {
    require_end(words, 4, "literal expression");
    expression.kind = form == "literal" ? ABI_EXPRESSION_LITERAL :
      ABI_EXPRESSION_INTEGRAL_VALUE;
    expression.value = signed_value(words[3]);
  } else if(form == "unary") {
    require_words(words, 5, "unary expression");
    require_end(words, 5, "unary expression");
    expression.kind = ABI_EXPRESSION_UNARY;
    expression.op = words[3];
    expression.expression_refs.push_back(words[4]);
  } else if(form == "binary") {
    require_words(words, 6, "binary expression");
    require_end(words, 6, "binary expression");
    expression.kind = ABI_EXPRESSION_BINARY;
    expression.op = words[3];
    expression.expression_refs.push_back(words[4]);
    expression.expression_refs.push_back(words[5]);
  } else if(form == "conditional") {
    require_words(words, 6, "conditional expression");
    require_end(words, 6, "conditional expression");
    expression.kind = ABI_EXPRESSION_CONDITIONAL;
    expression.expression_refs.assign(words.begin() + 3, words.begin() + 6);
  } else if(form == "pack") {
    require_end(words, 4, "pack expression");
    expression.kind = ABI_EXPRESSION_PACK_EXPANSION;
    expression.expression_refs.push_back(words[3]);
  } else if(form == "call") {
    expression.kind = ABI_EXPRESSION_CALL;
    expression.expression_refs.assign(words.begin() + 3, words.end());
  } else if(form == "conversion" || form == "cast") {
    require_words(words, 6, "conversion expression");
    expression.kind = form == "conversion" ? ABI_EXPRESSION_CONVERSION :
      ABI_EXPRESSION_CAST;
    expression.op = words[3];
    const std::size_t next = parse_type_at(words, 4, 0, &expression.type);
    require_end(words, next + 1, "conversion expression");
    expression.expression_refs.push_back(words[next]);
  } else if(form == "template-id") {
    expression.kind = ABI_EXPRESSION_TEMPLATE_ID;
    expression.text = words[3];
    parse_argument_refs(words, 4, &expression.argument_refs);
  } else if(form == "type-trait") {
    require_words(words, 5, "type-trait expression");
    expression.kind = ABI_EXPRESSION_TYPE_TRAIT;
    expression.text = words[3];
    parse_type_sequence(words, 4, 0, &expression.type_arguments);
  } else if(form == "sizeof-type") {
    require_end(words, parse_type_at(words, 3, 0, &expression.type),
                "sizeof expression");
    expression.kind = ABI_EXPRESSION_SIZEOF_TYPE;
  } else if(form == "member") {
    require_words(words, 6, "member expression");
    const std::size_t next = parse_type_at(words, 3, 0, &expression.type);
    require_words(words, next + 2, "member expression");
    expression.kind = ABI_EXPRESSION_MEMBER;
    expression.close_member_owner = fact_flag(words[next]);
    expression.text = words[next + 1];
    parse_argument_refs(words, next + 2, &expression.argument_refs);
  } else if(form == "object-member") {
    require_words(words, 6, "object member expression");
    expression.kind = ABI_EXPRESSION_OBJECT_MEMBER;
    expression.op = words[3];
    expression.expression_refs.push_back(words[4]);
    expression.text = words[5];
    parse_argument_refs(words, 6, &expression.argument_refs);
  } else if(form == "entity-reference" || form == "entity") {
    require_end(words, 4, "entity expression");
    expression.kind = form == "entity-reference" ?
      ABI_EXPRESSION_EXTERNAL_ENTITY : ABI_EXPRESSION_ENTITY;
    expression.entity_ref = words[3];
  } else {
    throw std::logic_error("unknown ABI expression form '" + form + "'");
  }
  fact_case->expressions.push_back(std::move(definition));
}

void parse_context_definition(const Words & words, AbiFactCase * fact_case)
{
  require_words(words, 4, "context definition");
  AbiContextDefinition definition;
  definition.id = words[1];
  if(words[2] == "raw") {
    require_end(words, 4, "raw context definition");
    definition.context.kind = ABI_CONTEXT_RAW;
    definition.context.fragment = words[3];
  } else if(words[2] == "function") {
    definition.context.kind = ABI_CONTEXT_FUNCTION;
    definition.context.function = parse_function_target(words, 2);
  } else {
    throw std::logic_error("unknown ABI context form '" + words[2] + "'");
  }
  fact_case->contexts.push_back(std::move(definition));
}

void parse_entity_definition(const Words & words, AbiFactCase * fact_case)
{
  require_words(words, 4, "entity definition");
  AbiEntityDefinition definition;
  definition.id = words[1];
  AbiEntityFact & entity = definition.entity;
  const std::string & form = words[2];
  if(form == "variable" || form == "internal-variable") {
    require_end(words, 4, "variable entity definition");
    entity.kind = ABI_ENTITY_FACT_VARIABLE;
    entity.internal_linkage = form == "internal-variable";
    entity.qualified_name = words[3];
  } else if(form == "symbol") {
    require_end(words, 4, "symbol entity definition");
    entity.kind = ABI_ENTITY_FACT_SYMBOL;
    entity.qualified_name = words[3];
  } else if(form == "function") {
    entity.kind = ABI_ENTITY_FACT_FUNCTION;
    entity.function = parse_function_target(words, 2);
  } else {
    throw std::logic_error("unknown ABI entity form '" + form + "'");
  }
  fact_case->entities.push_back(std::move(definition));
}

// ---- targets ----

void set_target(AbiFactCase * fact_case, AbiTargetRecord * target)
{
  if(fact_case->has_target) {
    throw std::logic_error("ABI case must contain exactly one target");
  }
  fact_case->has_target = true;
  fact_case->target = std::move(*target);
}

bool parse_target_words(const Words & words, AbiFactCase * fact_case)
{
  const std::string & form = words[0];
  AbiTargetRecord target;
  if(form == "type" || form == "typeinfo" || form == "vtable" || form == "vtt") {
    require_words(words, 2, "type target");
    require_end(words, parse_type_at(words, 1, 0, &target.type), "type target");
    target.kind = form == "type" ? ABI_TARGET_FACT_TYPE :
      form == "typeinfo" ? ABI_TARGET_FACT_TYPEINFO :
      form == "vtable" ? ABI_TARGET_FACT_VTABLE : ABI_TARGET_FACT_VTT;
  } else if(form == "variable") {
    require_words(words, 2, "variable target");
    require_end(words, 2, "variable target");
    target.kind = ABI_TARGET_FACT_VARIABLE;
    target.qualified_name = words[1];
  } else if(form == "c-function") {
    require_words(words, 2, "C function target");
    target.kind = ABI_TARGET_FACT_FUNCTION;
    target.c_linkage = true;
    target.function.kind = ABI_FUNCTION_TARGET_PATH;
    target.function.qualified_name = words[1];
    std::size_t next = 2;
    while(next < words.size()) {
      next = parse_path_operand(words, next, &target.function.path_operands);
    }
  } else if(form == "function") {
    target.kind = ABI_TARGET_FACT_FUNCTION;
    target.function = parse_function_target(words, 0);
  } else if(form == "construction-vtable") {
    require_words(words, 4, "construction-vtable target");
    const std::size_t offset = parse_type_at(words, 1, 0, &target.type);
    if(offset + 1 >= words.size()) {
      throw std::logic_error("construction-vtable needs an offset and base");
    }
    target.base_offset = decimal_index(words[offset]);
    require_end(words, parse_type_at(words, offset + 1, 0, &target.base_type),
                "construction-vtable target");
    target.kind = ABI_TARGET_FACT_CONSTRUCTION_VTABLE;
  } else if(form == "tls-wrapper") {
    require_words(words, 3, "TLS wrapper target");
    require_end(words, 3, "TLS wrapper target");
    if(words[1] != "variable") {
      throw std::logic_error("TLS wrapper target needs a variable");
    }
    target.kind = ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER;
    target.qualified_name = words[2];
  } else if(form == "virtual-base-thunk") {
    require_words(words, 4, "thunk target");
    target.kind = ABI_TARGET_FACT_VIRTUAL_BASE_THUNK;
    target.vcall_offset = signed_value(words[1]);
    if(words[2] != "function") {
      throw std::logic_error("thunk target needs a function");
    }
    target.function = parse_function_target(words, 2);
  } else if(form == "thunk") {
    require_words(words, 4, "thunk target");
    target.kind = ABI_TARGET_FACT_THUNK;
    target.this_adjust = signed_value(words[1]);
    std::size_t next = 2;
    if(words[next] != "function" && words[next] != "virtual-result") {
      target.has_result_adjust = true;
      target.result_adjust = signed_value(words[next++]);
    }
    if(next < words.size() && words[next] == "virtual-result") {
      require_words(words, next + 4, "virtual-result thunk");
      target.has_result_adjust = true;
      target.result_adjust_virtual = true;
      target.result_adjust = signed_value(words[next + 1]);
      target.result_vcall_offset = signed_value(words[next + 2]);
      next += 3;
    }
    if(next >= words.size() || words[next] != "function") {
      throw std::logic_error("thunk target needs a function");
    }
    target.function = parse_function_target(words, next);
  } else {
    return false;
  }
  set_target(fact_case, &target);
  return true;
}

// ---- function records ----

AbiFunctionQualifier parse_qualifier(const std::string & word)
{
  if(word == "const") return ABI_FUNCTION_QUALIFIER_CONST;
  if(word == "volatile") return ABI_FUNCTION_QUALIFIER_VOLATILE;
  if(word == "lvalue-ref" || word == "&") {
    return ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE;
  }
  if(word == "rvalue-ref" || word == "&&") {
    return ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE;
  }
  throw std::logic_error("unknown function qualifier '" + word + "'");
}

void parse_function_record_words(const Words & words, AbiFactCase * fact_case)
{
  AbiFunctionRecord function;
  const std::string & form = words[0];
  if(form == "name-source") {
    require_words(words, 2, "name-source record");
    if(words.size() > 3) {
      throw std::logic_error("extra words in name-source record");
    }
    function.kind = ABI_FUNCTION_RECORD_NAME_SOURCE;
    function.source_name = words[1];
    function.substitution = words.size() == 3 ? words[2] : "";
  } else if(form == "name-std") {
    if(words.size() > 2) {
      throw std::logic_error("extra words in name-std record");
    }
    function.kind = ABI_FUNCTION_RECORD_NAME_STD;
    function.standard_substitution = words.size() == 2 ? words[1] : "";
  } else if(form == "name-template") {
    require_words(words, 6, "name-template record");
    function.kind = ABI_FUNCTION_RECORD_NAME_TEMPLATE;
    function.name = words[1];
    function.substitution = words[2];
    function.complete_substitution = words[3];
    function.standard_substitution = is_dash(words[4]) ? "" : words[4];
    function.standard_substitution_includes_arguments = fact_flag(words[5]);
    parse_argument_refs(words, 6, &function.argument_refs);
  } else if(form == "function-template-arg") {
    require_words(words, 2, "function template argument record");
    require_end(words, 2, "function template argument record");
    function.kind = ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT;
    function.argument_refs.push_back(words[1]);
  } else if(form == "function-template-prefix") {
    require_words(words, 2, "function template prefix record");
    require_end(words, 2, "function template prefix record");
    function.kind = ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX;
    function.substitution = words[1];
  } else if(form == "namespace-lambda-context") {
    require_words(words, 2, "namespace lambda context record");
    function.kind = ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT;
    function.source_name = words[1];
    parse_argument_refs(words, 2, &function.namespace_qualifiers);
  } else if(form == "local-context") {
    require_words(words, 4, "local context record");
    function.kind = ABI_FUNCTION_RECORD_LOCAL_CONTEXT;
    function.context_ref = words[1];
    function.source_name = words[2];
    function.discriminator = words[3];
    parse_type_sequence(words, 4, 0, &function.types);
  } else if(form == "lambda-context") {
    require_words(words, 3, "lambda context record");
    function.kind = ABI_FUNCTION_RECORD_LAMBDA_CONTEXT;
    function.context_ref = words[1];
    function.discriminator = words[2];
    parse_type_sequence(words, 3, 0, &function.types);
  } else if(form == "terminal-source") {
    require_words(words, 2, "terminal record");
    require_end(words, 2, "terminal record");
    function.kind = ABI_FUNCTION_RECORD_TERMINAL;
    function.terminal.kind = ABI_TERMINAL_SOURCE;
    function.terminal.name = words[1];
  } else if(form == "terminal") {
    require_words(words, 2, "terminal record");
    require_end(words, 2, "terminal record");
    function.kind = ABI_FUNCTION_RECORD_TERMINAL;
    function.terminal.kind = ABI_TERMINAL_SPECIAL;
    if(!lookup_special_function(words[1], &function.terminal.special_function)) {
      throw std::logic_error("unknown ABI function terminal '" + words[1] + "'");
    }
  } else if(form == "operator-terminal") {
    require_words(words, 2, "operator terminal record");
    function.kind = ABI_FUNCTION_RECORD_TERMINAL;
    if(words[1] == "literal") {
      require_words(words, 3, "literal operator record");
      require_end(words, 3, "literal operator record");
      function.terminal.kind = ABI_TERMINAL_LITERAL_OPERATOR;
      function.terminal.name = words[2];
    } else {
      require_end(words, 2, "operator terminal record");
      function.terminal.kind = ABI_TERMINAL_OPERATOR;
      if(!lookup_operator(words[1], &function.terminal.operator_kind)) {
        throw std::logic_error("unknown ABI operator terminal '" + words[1] + "'");
      }
    }
  } else if(form == "conversion-terminal") {
    require_words(words, 2, "conversion terminal record");
    require_end(words, parse_type_at(words, 1, 0, &function.type),
                "conversion terminal record");
    function.kind = ABI_FUNCTION_RECORD_TERMINAL;
    function.terminal.kind = ABI_TERMINAL_CONVERSION;
  } else if(form == "variadic") {
    require_end(words, 1, "variadic record");
    function.kind = ABI_FUNCTION_RECORD_VARIADIC;
  } else if(form == "abi-tag") {
    require_words(words, 2, "ABI tag record");
    require_end(words, 2, "ABI tag record");
    function.kind = ABI_FUNCTION_RECORD_ABI_TAG;
    function.name = words[1];
  } else if(form == "qualifier" || form == "function-qualifier") {
    require_words(words, 2, "qualifier record");
    function.kind = ABI_FUNCTION_RECORD_QUALIFIER;
    for(std::size_t i = 1; i < words.size(); ++i) {
      function.qualifiers.push_back(parse_qualifier(words[i]));
    }
  } else if(form == "param" || form == "result") {
    require_words(words, 2, "function type record");
    require_end(words, parse_type_at(words, 1, 0, &function.type),
                "function type record");
    function.kind = form == "param" ? ABI_FUNCTION_RECORD_PARAMETER :
      ABI_FUNCTION_RECORD_RESULT;
  } else {
    throw std::logic_error("unknown ABI fact record '" + form + "'");
  }
  fact_case->function_records.push_back(std::move(function));
}

// ---- case completion ----

void collect_id(const std::string & id, std::set<std::string> * ids)
{
  if(!ids->insert(id).second) {
    throw std::logic_error("duplicate ABI definition '" + id + "'");
  }
}

// A bare operand word names a template argument when the case defines one by
// that id; otherwise it is a type reference resolved by the encoder.
void resolve_path_operands(const std::set<std::string> & argument_ids,
                           AbiFunctionTarget * target)
{
  for(std::size_t i = 0; i < target->path_operands.size(); ++i) {
    AbiFunctionPathOperand & operand = target->path_operands[i];
    if(operand.kind != ABI_FUNCTION_PATH_TYPE ||
       operand.type.kind != ABI_TYPE_NAME_OR_REFERENCE ||
       argument_ids.find(operand.type.name) == argument_ids.end()) {
      continue;
    }
    operand.kind = ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT;
    operand.argument_ref = operand.type.name;
    operand.type = AbiType();
  }
}

void resolve_case(AbiFactCase * fact_case)
{
  std::set<std::string> ids;
  std::set<std::string> argument_ids;
  for(std::size_t i = 0; i < fact_case->types.size(); ++i) {
    collect_id(fact_case->types[i].id, &ids);
  }
  for(std::size_t i = 0; i < fact_case->arguments.size(); ++i) {
    collect_id(fact_case->arguments[i].id, &ids);
    argument_ids.insert(fact_case->arguments[i].id);
  }
  for(std::size_t i = 0; i < fact_case->expressions.size(); ++i) {
    collect_id(fact_case->expressions[i].id, &ids);
  }
  for(std::size_t i = 0; i < fact_case->contexts.size(); ++i) {
    collect_id(fact_case->contexts[i].id, &ids);
  }
  for(std::size_t i = 0; i < fact_case->entities.size(); ++i) {
    collect_id(fact_case->entities[i].id, &ids);
  }
  if(!fact_case->has_target) {
    throw std::logic_error("ABI case must contain exactly one target");
  }
  resolve_path_operands(argument_ids, &fact_case->target.function);
  for(std::size_t i = 0; i < fact_case->contexts.size(); ++i) {
    resolve_path_operands(argument_ids, &fact_case->contexts[i].context.function);
  }
  for(std::size_t i = 0; i < fact_case->entities.size(); ++i) {
    resolve_path_operands(argument_ids, &fact_case->entities[i].entity.function);
  }
}

}  // namespace

void parse_fact_record_words(const std::vector<std::string> & words,
                             AbiFactCase * fact_case)
{
  if(words.empty()) {
    throw std::logic_error("empty ABI fact record");
  }
  const std::string & form = words[0];
  if(form == "let-type") {
    parse_type_definition(words, fact_case);
  } else if(form == "let-arg") {
    parse_argument_definition(words, fact_case);
  } else if(form == "let-expr") {
    parse_expression_definition(words, fact_case);
  } else if(form == "let-context") {
    parse_context_definition(words, fact_case);
  } else if(form == "let-entity") {
    parse_entity_definition(words, fact_case);
  } else if(!parse_target_words(words, fact_case)) {
    parse_function_record_words(words, fact_case);
  }
}

namespace {

// Streams cases out of one fact text so a batch file never needs more than
// one case in memory.
class FactCaseReader
{
public:
  explicit FactCaseReader(const std::string & text)
    : text_(text), position_(0), pending_label_(false)
  {
  }

  // Fills *fact_case with the next complete case; false at end of input.
  bool next_case(AbiFactCase * fact_case)
  {
    *fact_case = AbiFactCase();
    bool active = false;
    if(pending_label_) {
      fact_case->label = label_;
      pending_label_ = false;
      active = true;
    }
    while(position_ <= text_.size()) {
      const std::size_t end = text_.find('\n', position_);
      const std::size_t stop = end == std::string::npos ? text_.size() : end;
      const Words words = split_words(text_.substr(position_, stop - position_));
      position_ = stop + 1;
      if(words.empty()) {
        continue;
      }
      if(words[0] == "case") {
        require_words(words, 2, "case header");
        require_end(words, 2, "case header");
        if(active) {
          label_ = words[1];
          pending_label_ = true;
          resolve_case(fact_case);
          return true;
        }
        fact_case->label = words[1];
        active = true;
        continue;
      }
      active = true;
      parse_fact_record_words(words, fact_case);
    }
    if(!active) {
      return false;
    }
    resolve_case(fact_case);
    return true;
  }

private:
  const std::string & text_;
  std::size_t position_;
  bool pending_label_;
  std::string label_;
};

}  // namespace

AbiFactFile parse_fact_text(const std::string & text)
{
  AbiFactFile file;
  FactCaseReader reader(text);
  AbiFactCase fact_case;
  while(reader.next_case(&fact_case)) {
    file.cases.push_back(std::move(fact_case));
  }
  if(file.cases.empty()) {
    throw std::logic_error("ABI fact file contains no cases");
  }
  return file;
}

std::string mangle_fact_files(const std::vector<std::string> & input_paths)
{
  std::string output;
  for(std::size_t i = 0; i < input_paths.size(); ++i) {
    std::ifstream input(input_paths[i].c_str(), std::ios::binary);
    if(!input) {
      throw std::logic_error("unable to read ABI fact file '" + input_paths[i] +
        "'");
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    FactCaseReader reader(text);
    AbiFactCase fact_case;
    std::size_t cases = 0;
    while(reader.next_case(&fact_case)) {
      output += mangle_fact_case(fact_case);
      output += "\n";
      ++cases;
    }
    if(cases == 0) {
      throw std::logic_error("ABI fact file contains no cases");
    }
  }
  return output;
}

// ---------------------------------------------------------------------------
// Serializer: the canonical multiword spelling of every fact.

namespace {

std::string decimal_word(unsigned long long value)
{
  return std::to_string(value);
}

std::string signed_word(long long value)
{
  return std::to_string(value);
}

std::string flag_word(bool value)
{
  return value ? "yes" : "no";
}

std::string dash_or(const std::string & word)
{
  return word.empty() ? "-" : word;
}

void append_word(std::string * line, const std::string & word)
{
  if(!line->empty()) {
    *line += " ";
  }
  *line += word;
}

void append_words(std::string * line, const std::vector<std::string> & words)
{
  for(std::size_t i = 0; i < words.size(); ++i) {
    append_word(line, words[i]);
  }
}

std::string serialize_type(const AbiType & type);

std::string serialize_type_core(const AbiType & type)
{
  std::string line;
  switch(type.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
    return type.name;
  case ABI_TYPE_NAMED:
    return "named:" + type.name;
  case ABI_TYPE_BUILTIN:
    return builtin_type_info(type.builtin).word;
  case ABI_TYPE_TEMPLATE_PARAMETER:
    return std::string(type.substitutable ? "template-param-subst " :
      "template-param ") + decimal_word(type.index);
  case ABI_TYPE_POINTER:
    return "ptr " + serialize_type(type.types.at(0));
  case ABI_TYPE_LVALUE_REFERENCE:
    return "ref " + serialize_type(type.types.at(0));
  case ABI_TYPE_RVALUE_REFERENCE:
    return "rref " + serialize_type(type.types.at(0));
  case ABI_TYPE_PACK_EXPANSION:
    return "pack " + serialize_type(type.types.at(0));
  case ABI_TYPE_ARRAY:
    return "array " + type.array_bound.value + " " +
      serialize_type(type.types.at(0));
  case ABI_TYPE_VENDOR_QUALIFIED:
    return "vendor " + type.name + " " + serialize_type(type.types.at(0));
  case ABI_TYPE_BUILTIN_TRANSFORM:
    return "builtin-transform " + type.name + " " +
      serialize_type(type.types.at(0));
  case ABI_TYPE_FUNCTION:
    line = type.variadic ? "function-type-variadic" : "function-type";
    for(std::size_t i = 0; i < type.types.size(); ++i) {
      append_word(&line, serialize_type(type.types[i]));
    }
    return line;
  case ABI_TYPE_MEMBER_POINTER:
    return "member-pointer " + serialize_type(type.types.at(0)) + " " +
      serialize_type(type.types.at(1));
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
    line = "template " + type.name;
    append_words(&line, type.argument_refs);
    return line;
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION:
    line = "template-param-template " + decimal_word(type.index);
    append_words(&line, type.argument_refs);
    return line;
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
    line = "std-template " + type.standard_substitution + " " +
      flag_word(type.standard_substitution_includes_arguments) + " " + type.name;
    append_words(&line, type.argument_refs);
    return line;
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION:
    line = type.kind == ABI_TYPE_MEMBER ? "member " : "member-template ";
    line += serialize_type(type.types.at(0)) + " " + type.name;
    append_words(&line, type.argument_refs);
    return line;
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    return "decltype " + type.expression_ref;
  case ABI_TYPE_LAMBDA_CLOSURE:
    line = "lambda-closure " + type.context_ref + " " + type.discriminator;
    for(std::size_t i = 0; i < type.types.size(); ++i) {
      append_word(&line, serialize_type(type.types[i]));
    }
    return line;
  case ABI_TYPE_LOCAL_TYPE:
    return "local-type " + type.context_ref + " " + type.name + " " +
      type.discriminator;
  case ABI_TYPE_NAMESPACE_LAMBDA:
    line = "namespace-lambda " + type.name;
    append_words(&line, type.namespace_qualifiers);
    return line;
  }
  throw std::logic_error("cannot serialize unknown ABI type");
}

std::string serialize_type(const AbiType & type)
{
  std::string line = serialize_type_core(type);
  if(!type.abi_tags.empty()) {
    line = "tagged " + line;
    append_words(&line, type.abi_tags);
  }
  if(type.is_const) {
    line = "const " + line;
  }
  if(type.is_volatile) {
    line = "volatile " + line;
  }
  return line;
}

std::string serialize_terminal(const AbiTerminal & terminal, const AbiType & type)
{
  switch(terminal.kind) {
  case ABI_TERMINAL_SOURCE:
    return "terminal-source " + terminal.name;
  case ABI_TERMINAL_SPECIAL:
    return std::string("terminal ") +
      special_function_info(terminal.special_function).word;
  case ABI_TERMINAL_OPERATOR:
    return std::string("operator-terminal ") +
      operator_info(terminal.operator_kind).word;
  case ABI_TERMINAL_LITERAL_OPERATOR:
    return "operator-terminal literal " + terminal.name;
  case ABI_TERMINAL_CONVERSION:
    return "conversion-terminal " + serialize_type(type);
  }
  throw std::logic_error("cannot serialize unknown ABI terminal");
}

// Terminal word of a compact local target.
std::string compact_terminal_word(const AbiTerminal & terminal)
{
  if(terminal.kind == ABI_TERMINAL_OPERATOR &&
     terminal.operator_kind == ABI_OPERATOR_CALL) {
    return "operator-call";
  }
  if(terminal.kind == ABI_TERMINAL_SPECIAL) {
    return special_function_info(terminal.special_function).word;
  }
  if(terminal.kind == ABI_TERMINAL_SOURCE) {
    return terminal.name;
  }
  throw std::logic_error("compact local function targets take a call, "
    "constructor, destructor, or source-name terminal");
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
  std::string line;
  switch(target.kind) {
  case ABI_FUNCTION_TARGET_ENCODING:
    return "function encoding";
  case ABI_FUNCTION_TARGET_LOCAL:
    line = "function local " + target.context_ref + " " + target.source_name +
      " " + compact_terminal_word(target.terminal) + " " + target.discriminator;
    break;
  case ABI_FUNCTION_TARGET_LAMBDA:
    line = "function lambda " + target.context_ref + " " + target.discriminator +
      " " + compact_terminal_word(target.terminal);
    break;
  case ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA:
    line = "function namespace-lambda " + target.source_name + " " +
      compact_terminal_word(target.terminal);
    append_words(&line, target.namespace_qualifiers);
    return line;
  case ABI_FUNCTION_TARGET_PATH:
    line = "function path " + target.qualified_name;
    for(std::size_t i = 0; i < target.path_operands.size(); ++i) {
      append_word(&line, serialize_path_operand(target.path_operands[i]));
    }
    return line;
  }
  for(std::size_t i = 0; i < target.signature_parameter_types.size(); ++i) {
    append_word(&line, serialize_type(target.signature_parameter_types[i]));
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
    return "literal " + signed_word(expression.value);
  case ABI_EXPRESSION_INTEGRAL_VALUE:
    return "integral-value " + signed_word(expression.value);
  case ABI_EXPRESSION_UNARY:
    return "unary " + expression.op + " " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_BINARY:
    return "binary " + expression.op + " " + expression.expression_refs.at(0) +
      " " + expression.expression_refs.at(1);
  case ABI_EXPRESSION_CONDITIONAL:
    line = "conditional";
    append_words(&line, expression.expression_refs);
    return line;
  case ABI_EXPRESSION_PACK_EXPANSION:
    return "pack " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_CALL:
    line = "call";
    append_words(&line, expression.expression_refs);
    return line;
  case ABI_EXPRESSION_CONVERSION:
  case ABI_EXPRESSION_CAST:
    return std::string(expression.kind == ABI_EXPRESSION_CONVERSION ?
      "conversion " : "cast ") + expression.op + " " +
      serialize_type(expression.type) + " " + expression.expression_refs.at(0);
  case ABI_EXPRESSION_TEMPLATE_ID:
    line = "template-id " + expression.text;
    append_words(&line, expression.argument_refs);
    return line;
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
    append_words(&line, expression.argument_refs);
    return line;
  case ABI_EXPRESSION_OBJECT_MEMBER:
    line = "object-member " + expression.op + " " +
      expression.expression_refs.at(0) + " " + expression.text;
    append_words(&line, expression.argument_refs);
    return line;
  case ABI_EXPRESSION_EXTERNAL_ENTITY:
    return "entity-reference " + expression.entity_ref;
  case ABI_EXPRESSION_ENTITY:
    return "entity " + expression.entity_ref;
  }
  throw std::logic_error("cannot serialize unknown ABI expression");
}

std::string serialize_argument(const AbiTemplateArgument & argument)
{
  std::string line;
  switch(argument.kind) {
  case ABI_TEMPLATE_ARGUMENT_TYPE:
    return "type " + serialize_type(argument.type);
  case ABI_TEMPLATE_ARGUMENT_VALUE:
    return "value " + serialize_type(argument.value_type) + " " +
      signed_word(argument.value);
  case ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE:
    return "dependent-value " + argument.type.name + " " +
      serialize_type(argument.value_type) + " " + signed_word(argument.value);
  case ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE:
    return "untyped-value " + signed_word(argument.value);
  case ABI_TEMPLATE_ARGUMENT_EXPRESSION:
    return "expression " + argument.expression_ref;
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY:
    return "template-entity " + argument.name;
  case ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY:
    return "member-template-entity " + serialize_type(argument.type) + " " +
      argument.name + " " + argument.substitution;
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE:
    return "template-param-template " + decimal_word(argument.index);
  case ABI_TEMPLATE_ARGUMENT_ENTITY:
    return "entity-address " + argument.entity_ref;
  case ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY:
    line = "member-external-address " + argument.symbol + " " +
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
  case ABI_TEMPLATE_ARGUMENT_PACK:
    line = "pack";
    append_words(&line, argument.argument_refs);
    return line;
  }
  throw std::logic_error("cannot serialize unknown ABI template argument");
}

std::string serialize_entity(const AbiEntityFact & entity)
{
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return "symbol " + entity.qualified_name;
  }
  if(entity.kind == ABI_ENTITY_FACT_FUNCTION) {
    return serialize_function_target(entity.function);
  }
  return (entity.internal_linkage ? "internal-variable " : "variable ") +
    entity.qualified_name;
}

std::string serialize_function_record(const AbiFunctionRecord & function)
{
  std::string line;
  switch(function.kind) {
  case ABI_FUNCTION_RECORD_NAME_SOURCE:
    line = "name-source " + dash_or(function.source_name);
    if(!function.substitution.empty()) {
      append_word(&line, function.substitution);
    }
    return line;
  case ABI_FUNCTION_RECORD_NAME_STD:
    return function.standard_substitution.empty() ? "name-std" :
      "name-std " + function.standard_substitution;
  case ABI_FUNCTION_RECORD_NAME_TEMPLATE:
    line = "name-template " + function.name + " " +
      dash_or(function.substitution) + " " +
      dash_or(function.complete_substitution) + " " +
      dash_or(function.standard_substitution) + " " +
      flag_word(function.standard_substitution_includes_arguments);
    append_words(&line, function.argument_refs);
    return line;
  case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT:
    return "function-template-arg " + function.argument_refs.at(0);
  case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX:
    return "function-template-prefix " + function.substitution;
  case ABI_FUNCTION_RECORD_LOCAL_CONTEXT:
  case ABI_FUNCTION_RECORD_LAMBDA_CONTEXT:
    line = function.kind == ABI_FUNCTION_RECORD_LOCAL_CONTEXT ?
      "local-context " + function.context_ref + " " + function.source_name +
      " " + function.discriminator :
      "lambda-context " + function.context_ref + " " + function.discriminator;
    for(std::size_t i = 0; i < function.types.size(); ++i) {
      append_word(&line, serialize_type(function.types[i]));
    }
    return line;
  case ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT:
    line = "namespace-lambda-context " + function.source_name;
    append_words(&line, function.namespace_qualifiers);
    return line;
  case ABI_FUNCTION_RECORD_TERMINAL:
    return serialize_terminal(function.terminal, function.type);
  case ABI_FUNCTION_RECORD_VARIADIC:
    return "variadic";
  case ABI_FUNCTION_RECORD_ABI_TAG:
    return "abi-tag " + function.name;
  case ABI_FUNCTION_RECORD_QUALIFIER:
    line = "qualifier";
    for(std::size_t i = 0; i < function.qualifiers.size(); ++i) {
      const AbiFunctionQualifier qualifier = function.qualifiers[i];
      append_word(&line, qualifier == ABI_FUNCTION_QUALIFIER_CONST ? "const" :
        qualifier == ABI_FUNCTION_QUALIFIER_VOLATILE ? "volatile" :
        qualifier == ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE ? "lvalue-ref" :
        "rvalue-ref");
    }
    return line;
  case ABI_FUNCTION_RECORD_PARAMETER:
    return "param " + serialize_type(function.type);
  case ABI_FUNCTION_RECORD_RESULT:
    return "result " + serialize_type(function.type);
  }
  throw std::logic_error("cannot serialize unknown ABI function record");
}

std::string serialize_target(const AbiTargetRecord & target)
{
  std::string line;
  switch(target.kind) {
  case ABI_TARGET_FACT_TYPE:
    return "type " + serialize_type(target.type);
  case ABI_TARGET_FACT_VARIABLE:
    return "variable " + target.qualified_name;
  case ABI_TARGET_FACT_TYPEINFO:
    return "typeinfo " + serialize_type(target.type);
  case ABI_TARGET_FACT_VTABLE:
    return "vtable " + serialize_type(target.type);
  case ABI_TARGET_FACT_VTT:
    return "vtt " + serialize_type(target.type);
  case ABI_TARGET_FACT_CONSTRUCTION_VTABLE:
    return "construction-vtable " + serialize_type(target.type) + " " +
      decimal_word(target.base_offset) + " " + serialize_type(target.base_type);
  case ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER:
    return "tls-wrapper variable " + target.qualified_name;
  case ABI_TARGET_FACT_VIRTUAL_BASE_THUNK:
    return "virtual-base-thunk " + signed_word(target.vcall_offset) + " " +
      serialize_function_target(target.function);
  case ABI_TARGET_FACT_THUNK:
    line = "thunk " + signed_word(target.this_adjust);
    if(target.has_result_adjust && target.result_adjust_virtual) {
      append_word(&line, "virtual-result");
      append_word(&line, signed_word(target.result_adjust));
      append_word(&line, signed_word(target.result_vcall_offset));
    } else if(target.has_result_adjust) {
      append_word(&line, signed_word(target.result_adjust));
    }
    append_word(&line, serialize_function_target(target.function));
    return line;
  case ABI_TARGET_FACT_FUNCTION:
    if(!target.c_linkage) {
      return serialize_function_target(target.function);
    }
    line = "c-function " + target.function.qualified_name;
    for(std::size_t i = 0; i < target.function.path_operands.size(); ++i) {
      append_word(&line, serialize_path_operand(target.function.path_operands[i]));
    }
    return line;
  }
  throw std::logic_error("cannot serialize unknown ABI target");
}

void append_line(std::string * output, const std::string & line)
{
  *output += line;
  *output += "\n";
}

}  // namespace

std::string serialize_fact_file(const AbiFactFile & file)
{
  std::string output;
  for(std::size_t i = 0; i < file.cases.size(); ++i) {
    const AbiFactCase & fact_case = file.cases[i];
    if(!fact_case.label.empty()) {
      append_line(&output, "case " + fact_case.label);
    } else if(i != 0) {
      throw std::logic_error("only the first ABI case may be unlabeled");
    }
    for(std::size_t j = 0; j < fact_case.types.size(); ++j) {
      append_line(&output, "let-type " + fact_case.types[j].id + " " +
        serialize_type(fact_case.types[j].type));
    }
    for(std::size_t j = 0; j < fact_case.arguments.size(); ++j) {
      append_line(&output, "let-arg " + fact_case.arguments[j].id + " " +
        serialize_argument(fact_case.arguments[j].argument));
    }
    for(std::size_t j = 0; j < fact_case.expressions.size(); ++j) {
      append_line(&output, "let-expr " + fact_case.expressions[j].id + " " +
        serialize_expression(fact_case.expressions[j].expression));
    }
    for(std::size_t j = 0; j < fact_case.contexts.size(); ++j) {
      const AbiLocalContext & context = fact_case.contexts[j].context;
      append_line(&output, "let-context " + fact_case.contexts[j].id + " " +
        (context.kind == ABI_CONTEXT_RAW ? "raw " + context.fragment :
         serialize_function_target(context.function)));
    }
    for(std::size_t j = 0; j < fact_case.entities.size(); ++j) {
      append_line(&output, "let-entity " + fact_case.entities[j].id + " " +
        serialize_entity(fact_case.entities[j].entity));
    }
    if(fact_case.has_target) {
      append_line(&output, serialize_target(fact_case.target));
    }
    for(std::size_t j = 0; j < fact_case.function_records.size(); ++j) {
      append_line(&output,
                  serialize_function_record(fact_case.function_records[j]));
    }
  }
  return output;
}

}  // namespace abi_mangle
