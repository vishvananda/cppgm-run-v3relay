#include "abi_mangle_encoder.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace abi_mangle {
namespace {

const std::size_t kMaximumDefinitionDepth = 65536;

void check_depth(std::size_t depth)
{
  if(depth > kMaximumDefinitionDepth) {
    throw std::logic_error("ABI definition graph is too deep");
  }
}

std::string number_word(unsigned long long value)
{
  std::ostringstream output;
  output << value;
  return output.str();
}

std::string strip_scope(const std::string & name)
{
  return name.compare(0, 2, "::") == 0 ? name.substr(2) : name;
}

std::string base_key(const std::string & prefix, const std::string & value)
{
  return prefix + "(" + value + ")";
}

std::string source_name(const std::string & name)
{
  return std::to_string(name.size()) + name;
}

std::string tags_key(const std::vector<std::string> & tags)
{
  std::vector<std::string> sorted = tags;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string key;
  for(std::size_t i = 0; i < sorted.size(); ++i) {
    key += "B" + sorted[i];
  }
  return key;
}

bool lookup_substitution(SubstitutionTable * table, const std::string & key,
                         std::string * result)
{
  return !key.empty() && table->lookup(key, result);
}

std::string cv_key(const AbiType & type, const std::string & child)
{
  std::string key;
  if(type.is_volatile) {
    key += "V";
  }
  if(type.is_const) {
    key += "K";
  }
  return key.empty() ? child : key + "(" + child + ")";
}

}  // namespace

bool SubstitutionTable::lookup(const std::string & key,
                               std::string * spelling) const
{
  const std::map<std::string, std::size_t>::const_iterator it = indexes_.find(key);
  if(it == indexes_.end()) {
    return false;
  }
  if(spelling) {
    *spelling = sequence_id(it->second);
  }
  return true;
}

void SubstitutionTable::add(const std::string & key)
{
  if(key.empty() || indexes_.find(key) != indexes_.end()) {
    return;
  }
  const std::size_t index = keys_.size();
  keys_.push_back(key);
  indexes_[key] = index;
}

std::string SubstitutionTable::sequence_id(std::size_t index) const
{
  if(index == 0) {
    return "S_";
  }
  --index;
  std::string result;
  do {
    const std::size_t digit = index % 36;
    result.insert(result.begin(),
      static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10));
    index /= 36;
  } while(index != 0);
  return "S" + result + "_";
}

Mangler::Mangler(const AbiDefinitionTable & definitions)
  : definitions_(definitions)
{
}

const AbiDefinitionRecord * Mangler::definition(const std::string & id) const
{
  return definitions_.find(id);
}

const AbiType * Mangler::type_definition(const std::string & id) const
{
  const AbiDefinitionRecord * record = definition(id);
  if(!record || record->kind != ABI_DEFINITION_TYPE) {
    return 0;
  }
  return &record->type;
}

const AbiTemplateArgument * Mangler::argument_definition(
  const std::string & id) const
{
  const AbiDefinitionRecord * record = definition(id);
  if(!record || record->kind != ABI_DEFINITION_TEMPLATE_ARGUMENT) {
    return 0;
  }
  return &record->template_argument;
}

const AbiDependentExpression * Mangler::expression_definition(
  const std::string & id) const
{
  const AbiDefinitionRecord * record = definition(id);
  if(!record || record->kind != ABI_DEFINITION_EXPRESSION) {
    return 0;
  }
  return &record->expression;
}

const AbiEntityFact * Mangler::entity_definition(const std::string & id) const
{
  const AbiDefinitionRecord * record = definition(id);
  if(!record || record->kind != ABI_DEFINITION_ENTITY) {
    return 0;
  }
  return &record->entity;
}

std::string Mangler::builtin_code(const AbiType & type) const
{
  const std::string & name = type.name;
  if(name == "void") return "v";
  if(name == "bool") return "b";
  if(name == "char") return "c";
  if(name == "schar") return "a";
  if(name == "uchar") return "h";
  if(name == "short") return "s";
  if(name == "ushort") return "t";
  if(name == "int") return "i";
  if(name == "uint") return "j";
  if(name == "long") return "l";
  if(name == "ulong") return "m";
  if(name == "longlong") return "x";
  if(name == "ulonglong") return "y";
  if(name == "int128") return "n";
  if(name == "uint128") return "o";
  if(name == "float") return "f";
  if(name == "double") return "d";
  if(name == "longdouble") return "e";
  if(name == "float128") return "g";
  if(name == "wchar") return "w";
  if(name == "char16") return "Ds";
  if(name == "char32") return "Di";
  if(name == "nullptr") return "Dn";
  if(name == "auto") return "Da";
  if(name == "complex-float") return "Cf";
  if(name == "complex-double") return "Cd";
  if(name == "complex-longdouble") return "Ce";
  throw std::logic_error("unknown ABI builtin type '" + name + "'");
}

std::string Mangler::mangle_builtin(const AbiType & type) const
{
  return builtin_code(type);
}

std::string Mangler::type_name_key(const AbiType & type) const
{
  return "name:" + strip_scope(type.name) + tags_key(type.abi_tags);
}

std::string Mangler::key_of_type(const AbiType & type)
{
  return key_of_type_impl(type, 0);
}

std::string Mangler::type_key_ref(const std::string & ref, std::size_t depth)
{
  const AbiType * type = type_definition(ref);
  if(!type) {
    AbiType named;
    named.kind = ABI_TYPE_NAMED;
    named.name = ref;
    return key_of_type_impl(named, depth + 1);
  }
  const std::map<std::string, std::string>::const_iterator cached =
    type_key_cache_.find(ref);
  if(cached != type_key_cache_.end()) {
    return cached->second;
  }
  if(active_type_keys_.find(ref) != active_type_keys_.end()) {
    throw std::logic_error("cyclic ABI type definition");
  }
  active_type_keys_[ref] = true;
  try {
    const std::string key = key_of_type_impl(*type, depth + 1);
    active_type_keys_.erase(ref);
    type_key_cache_[ref] = key;
    return key;
  } catch(...) {
    active_type_keys_.erase(ref);
    throw;
  }
}

std::string Mangler::key_of_type_impl(const AbiType & input,
                                      std::size_t depth)
{
  check_depth(depth);
  if(input.is_const || input.is_volatile) {
    AbiType base = input;
    base.is_const = false;
    base.is_volatile = false;
    return cv_key(input, key_of_type_impl(base, depth + 1));
  }
  if(input.kind == ABI_TYPE_NAME_OR_REFERENCE) {
    if(type_definition(input.name)) {
      return type_key_ref(input.name, depth);
    }
    AbiType named = input;
    named.kind = ABI_TYPE_NAMED;
    return key_of_type_impl(named, depth + 1);
  }
  switch(input.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
    throw std::logic_error("unresolved ABI type reference");
  case ABI_TYPE_BUILTIN:
    return "";
  case ABI_TYPE_NAMED:
    return type_name_key(input);
  case ABI_TYPE_TEMPLATE_PARAMETER:
    return input.substitutable ? "Tsub(" + number_word(input.index) + ")" : "";
  case ABI_TYPE_POINTER:
    return base_key("P", key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_LVALUE_REFERENCE:
    return base_key("R", key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_RVALUE_REFERENCE:
    return base_key("O", key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_CV:
    return cv_key(input, key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_PACK_EXPANSION:
    return base_key("Dp", key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_VENDOR_QUALIFIED:
    return base_key("U" + number_word(input.name.size()) + input.name,
      key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_ARRAY:
    return base_key("A" + input.array_bound.value,
      key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_BUILTIN_TRANSFORM:
    return base_key("u" + number_word(input.name.size()) + input.name,
      key_of_type_impl(input.types.at(0), depth + 1));
  case ABI_TYPE_FUNCTION: {
    std::string key = "F";
    for(std::size_t i = 0; i < input.types.size(); ++i) {
      key += key_of_type_impl(input.types[i], depth + 1) + ";";
    }
    return key + (input.variadic ? "z" : "") + "E";
  }
  case ABI_TYPE_MEMBER_POINTER:
    return "M(" + key_of_type_impl(input.types.at(0), depth + 1) + "," +
      key_of_type_impl(input.types.at(1), depth + 1) + ")";
  case ABI_TYPE_TEMPLATE_SPECIALIZATION: {
    std::string key = "template:" + strip_scope(input.name) + "<";
    for(std::size_t i = 0; i < input.argument_refs.size(); ++i) {
      key += argument_key_ref(input.argument_refs[i], depth + 1) + ";";
    }
    return key + ">" + tags_key(input.abi_tags);
  }
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION: {
    std::string key = "TT(" + number_word(input.index) + ")<";
    for(std::size_t i = 0; i < input.argument_refs.size(); ++i) {
      key += argument_key_ref(input.argument_refs[i], depth + 1) + ";";
    }
    return key + ">";
  }
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
    return "std:" + input.standard_substitution + ":" +
      (input.standard_substitution_includes_arguments ? "yes" : "no") + ":" +
      strip_scope(input.name);
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION: {
    std::string key = input.kind == ABI_TYPE_MEMBER ? "member:" : "member-template:";
    key += key_of_type_impl(input.types.at(0), depth + 1) + ":" + input.name;
    for(std::size_t i = 0; i < input.argument_refs.size(); ++i) {
      key += ":" + argument_key_ref(input.argument_refs[i], depth + 1);
    }
    return key;
  }
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    return "DT(" + expression_key_ref(input.expression_ref, depth + 1) + ")";
  case ABI_TYPE_LAMBDA_CLOSURE: {
    std::string key = "lambda:" + input.context_ref + ":" +
      input.discriminator;
    for(std::size_t i = 0; i < input.types.size(); ++i) {
      key += ":" + key_of_type_impl(input.types[i], depth + 1);
    }
    return key;
  }
  case ABI_TYPE_LOCAL_TYPE:
    return "local:" + input.context_ref + ":" + input.name + ":" +
      input.discriminator;
  case ABI_TYPE_NAMESPACE_LAMBDA: {
    std::string key = "namespace-lambda:" + input.name;
    for(std::size_t i = 0; i < input.namespace_qualifiers.size(); ++i) {
      key += ":" + input.namespace_qualifiers[i];
    }
    return key;
  }
  }
  throw std::logic_error("unknown ABI type kind");
}

std::string Mangler::key_of_argument(const AbiTemplateArgument & argument)
{
  return key_of_argument_impl(argument, 0);
}

std::string Mangler::argument_key_ref(const std::string & ref,
                                      std::size_t depth)
{
  const AbiTemplateArgument * argument = argument_definition(ref);
  if(!argument) {
    throw std::logic_error("unknown ABI template argument '" + ref + "'");
  }
  const std::map<std::string, std::string>::const_iterator cached =
    argument_key_cache_.find(ref);
  if(cached != argument_key_cache_.end()) {
    return cached->second;
  }
  const std::string key = key_of_argument_impl(*argument, depth + 1);
  argument_key_cache_[ref] = key;
  return key;
}

std::string Mangler::key_of_argument_impl(const AbiTemplateArgument & argument,
                                          std::size_t depth)
{
  check_depth(depth);
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_TYPE) {
    return base_key("arg-type", key_of_type_impl(argument.type, depth + 1));
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_VALUE) {
    return "arg-value(" + builtin_code(argument.value_type) + ":" +
      number_word(static_cast<unsigned long long>(argument.value)) + ")";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE) {
    return "arg-dependent(" + type_key_ref(argument.expression_ref, depth + 1) +":" +
      builtin_code(argument.value_type) + ":" +
      number_word(static_cast<unsigned long long>(argument.value)) + ")";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_ENTITY) {
    const AbiEntityFact * entity = entity_definition(argument.entity_ref);
    if(!entity) {
      throw std::logic_error("unknown ABI entity '" + argument.entity_ref + "'");
    }
    return base_key("arg-entity", key_of_entity_impl(*entity, depth + 1));
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY) {
    return "arg-member-external(" + argument.symbol + ")";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_EXPRESSION) {
    return base_key("arg-expression", expression_key_ref(argument.expression_ref,
      depth + 1));
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE) {
    return "arg-template-param(" + number_word(argument.index) + ")";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY) {
    return "arg-template(" + strip_scope(argument.name) + ")";
  }
  std::string key = "arg:" + number_word(static_cast<unsigned long long>(argument.kind));
  for(std::size_t i = 0; i < argument.argument_refs.size(); ++i) {
    key += ":" + argument_key_ref(argument.argument_refs[i], depth + 1);
  }
  return key;
}

std::string Mangler::key_of_expression(const AbiDependentExpression & expression)
{
  return key_of_expression_impl(expression, 0);
}

std::string Mangler::expression_key_ref(const std::string & ref,
                                        std::size_t depth)
{
  const AbiDependentExpression * expression = expression_definition(ref);
  if(!expression) {
    throw std::logic_error("unknown ABI expression '" + ref + "'");
  }
  const std::map<std::string, std::string>::const_iterator cached =
    expression_key_cache_.find(ref);
  if(cached != expression_key_cache_.end()) {
    return cached->second;
  }
  const std::string key = key_of_expression_impl(*expression, depth + 1);
  expression_key_cache_[ref] = key;
  return key;
}

std::string Mangler::key_of_expression_impl(
  const AbiDependentExpression & expression, std::size_t depth)
{
  check_depth(depth);
  std::string key = "expr:" + number_word(static_cast<unsigned long long>(
    expression.kind));
  key += ":" + expression.op + ":" + expression.text;
  if(expression.kind == ABI_EXPRESSION_TEMPLATE_PARAMETER ||
     expression.kind == ABI_EXPRESSION_FUNCTION_PARAMETER) {
    return key + ":" + number_word(expression.index);
  }
  for(std::size_t i = 0; i < expression.expression_refs.size(); ++i) {
    key += ":" + expression_key_ref(expression.expression_refs[i], depth + 1);
  }
  for(std::size_t i = 0; i < expression.type_arguments.size(); ++i) {
    key += ":" + key_of_type_impl(expression.type_arguments[i], depth + 1);
  }
  return key;
}

std::string Mangler::key_of_entity(const AbiEntityFact & entity)
{
  return key_of_entity_impl(entity, 0);
}

std::string Mangler::key_of_entity_impl(const AbiEntityFact & entity,
                                        std::size_t depth)
{
  check_depth(depth);
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return "symbol:" + entity.qualified_name;
  }
  if(entity.kind == ABI_ENTITY_FACT_VARIABLE) {
    return "variable:" + strip_scope(entity.qualified_name) +
      (entity.internal_linkage ? ":internal" : "");
  }
  std::string key = "function:" + strip_scope(entity.function.qualified_name);
  for(std::size_t i = 0; i < entity.function.path_operands.size(); ++i) {
    const AbiFunctionPathOperand & operand = entity.function.path_operands[i];
    key += ":" + (operand.kind == ABI_FUNCTION_PATH_TYPE ?
      key_of_type_impl(operand.type, depth + 1) : operand.argument_ref);
  }
  return key;
}

std::string Mangler::mangle_type(const AbiType & type)
{
  const std::string linear = mangle_long_type_chain(type);
  if(!linear.empty()) {
    return linear;
  }
  return mangle_type_impl(type, 0);
}

std::string Mangler::mangle_long_type_chain(const AbiType & input)
{
  const AbiType * current = &input;
  AbiType unresolved;
  std::vector<std::string> prefixes;
  std::map<std::string, bool> seen;
  std::size_t steps = 0;
  while(true) {
    if(current->kind == ABI_TYPE_NAME_OR_REFERENCE) {
      const AbiType * referenced = type_definition(current->name);
      if(!referenced) {
        unresolved = *current;
        unresolved.kind = ABI_TYPE_NAMED;
        current = &unresolved;
        break;
      }
      if(!seen.insert(std::make_pair(current->name, true)).second) {
        throw std::logic_error("cyclic ABI type definition");
      }
      current = referenced;
      ++steps;
      continue;
    }
    if(current->is_const || current->is_volatile || current->types.size() != 1) {
      break;
    }
    if(current->kind == ABI_TYPE_POINTER) {
      prefixes.push_back("P");
    } else if(current->kind == ABI_TYPE_LVALUE_REFERENCE) {
      prefixes.push_back("R");
    } else if(current->kind == ABI_TYPE_RVALUE_REFERENCE) {
      prefixes.push_back("O");
    } else {
      break;
    }
    current = &current->types[0];
    ++steps;
  }
  if(steps <= 256) {
    return std::string();
  }
  std::string result = mangle_type_impl(*current, 0);
  for(std::size_t i = prefixes.size(); i > 0; --i) {
    result = prefixes[i - 1] + result;
  }
  return result;
}

std::string Mangler::mangle_type_impl(const AbiType & input, std::size_t depth)
{
  check_depth(depth);
  if(input.is_const || input.is_volatile) {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) {
      return spelling;
    }
    AbiType base = input;
    base.is_const = false;
    base.is_volatile = false;
    std::string result;
    if(input.is_volatile) {
      result += "V";
    }
    if(input.is_const) {
      result += "K";
    }
    result += mangle_type_impl(base, depth + 1);
    substitutions_.add(key);
    return result;
  }
  if(input.kind == ABI_TYPE_NAME_OR_REFERENCE) {
    const AbiType * definition_type = type_definition(input.name);
    if(definition_type) {
      if(active_type_mangles_.find(input.name) != active_type_mangles_.end()) {
        throw std::logic_error("cyclic ABI type definition");
      }
      active_type_mangles_[input.name] = true;
      try {
        const std::string result = mangle_type_impl(*definition_type, depth + 1);
        active_type_mangles_.erase(input.name);
        return result;
      } catch(...) {
        active_type_mangles_.erase(input.name);
        throw;
      }
    }
    AbiType named = input;
    named.kind = ABI_TYPE_NAMED;
    return mangle_type_impl(named, depth + 1);
  }
  if(input.kind == ABI_TYPE_CV) {
    if(input.types.size() != 1) {
      throw std::logic_error("cv ABI type needs one operand");
    }
    AbiType cv = input.types[0];
    cv.is_const = cv.is_const || input.is_const;
    cv.is_volatile = cv.is_volatile || input.is_volatile;
    return mangle_type_impl(cv, depth + 1);
  }
  switch(input.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
    throw std::logic_error("unresolved ABI type reference");
  case ABI_TYPE_BUILTIN:
    return mangle_builtin(input);
  case ABI_TYPE_NAMED:
    return mangle_named_type(input, depth + 1);
  case ABI_TYPE_TEMPLATE_PARAMETER: {
    std::string result = "T";
    if(input.index != 0) {
      result += number_word(static_cast<unsigned long long>(input.index - 1));
    }
    result += "_";
    if(input.substitutable) {
      const std::string key = key_of_type_impl(input, depth + 1);
      std::string spelling;
      if(lookup_substitution(&substitutions_, key, &spelling)) {
        return spelling;
      }
      substitutions_.add(key);
    }
    return result;
  }
  case ABI_TYPE_POINTER: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string result = "P" + mangle_type_impl(input.types.at(0), depth + 1);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_LVALUE_REFERENCE:
  case ABI_TYPE_RVALUE_REFERENCE: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string prefix = input.kind == ABI_TYPE_LVALUE_REFERENCE ? "R" : "O";
    const std::string result = prefix + mangle_type_impl(input.types.at(0), depth + 1);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_PACK_EXPANSION: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string result = "Dp" + mangle_type_impl(input.types.at(0), depth + 1);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_VENDOR_QUALIFIED: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string result = "U" + number_word(input.name.size()) + input.name +
      mangle_type_impl(input.types.at(0), depth + 1);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_ARRAY: {
    if(input.array_bound.kind != ABI_ARRAY_BOUND_VALUE) {
      throw std::logic_error("unsupported non-value array bound");
    }
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string result = "A" + input.array_bound.value + "_" +
      mangle_type_impl(input.types.at(0), depth + 1);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_BUILTIN_TRANSFORM: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string result = "u" + number_word(input.name.size()) + input.name +
      "I" + mangle_type_impl(input.types.at(0), depth + 1) + "E";
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_FUNCTION: {
    if(input.types.empty()) {
      throw std::logic_error("function ABI type needs a result");
    }
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    std::string result = "F";
    result += mangle_type_impl(input.types[0], depth + 1);
    if(input.types.size() == 1) {
      result += "v";
    } else {
      for(std::size_t i = 1; i < input.types.size(); ++i) {
        result += mangle_type_impl(input.types[i], depth + 1);
      }
    }
    if(input.variadic) result += "z";
    result += "E";
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_MEMBER_POINTER:
    return mangle_member_type(input, depth + 1);
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION:
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
    return mangle_template_type(input, depth + 1);
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION:
    return mangle_member_type(input, depth + 1);
  case ABI_TYPE_CV:
    throw std::logic_error("invalid cv ABI type");
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    throw std::logic_error("unsupported target");
  case ABI_TYPE_LAMBDA_CLOSURE: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    std::string result = mangle_context(input.context_ref) + "Ul";
    if(input.types.empty()) {
      result += "v";
    } else {
      for(std::size_t i = 0; i < input.types.size(); ++i) {
        result += mangle_type(input.types[i]);
      }
    }
    result += "E" + input.discriminator + "_";
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_LOCAL_TYPE: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    const std::string result = mangle_context(input.context_ref) +
      source_name(input.name) + mangle_local_discriminator(input.discriminator);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_NAMESPACE_LAMBDA: {
    const std::string key = key_of_type_impl(input, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    std::string result = "N";
    for(std::size_t i = 0; i < input.namespace_qualifiers.size(); ++i) {
      result += source_name(input.namespace_qualifiers[i]);
    }
    result += source_name(input.name) + "E";
    substitutions_.add(key);
    return result;
  }
  }
  throw std::logic_error("unknown ABI type kind");
}

std::string Mangler::mangle_named_type(const AbiType & type, std::size_t depth)
{
  const std::string key = key_of_type_impl(type, depth + 1);
  std::string spelling;
  if(lookup_substitution(&substitutions_, key, &spelling)) {
    return spelling;
  }
  // Function-name prefixes and named types share the ABI substitution
  // sequence, although the fact model records the former by source spelling
  // and the latter by a structural key.
  if(type.abi_tags.empty() &&
     lookup_substitution(&substitutions_, strip_scope(type.name), &spelling)) {
    return spelling;
  }
  const std::string result = mangle_qualified_name(type.name, false,
    type.abi_tags);
  substitutions_.add(key);
  return result;
}

std::string mangle_type_body_without_outer_name(const std::string & spelling)
{
  if(spelling.size() >= 2 && spelling[0] == 'N' && spelling[spelling.size() - 1] == 'E') {
    return spelling.substr(1, spelling.size() - 2);
  }
  return spelling;
}

bool nested_name(const std::string & qualified_name)
{
  const std::string name = strip_scope(qualified_name);
  std::size_t count = 1;
  for(std::size_t i = 0; i + 1 < name.size(); ++i) {
    if(name[i] == ':' && name[i + 1] == ':') {
      ++count;
      ++i;
    }
  }
  return (count > 1 && name.compare(0, 5, "std::") != 0) ||
    (count > 2 && name.compare(0, 5, "std::") == 0);
}

std::string Mangler::mangle_template_name(const std::string & qualified_name)
{
  const bool nested = nested_name(qualified_name);
  std::string spelling;
  if(substitutions_.lookup(strip_scope(qualified_name), &spelling)) {
    return nested ? "N" + spelling : spelling;
  }
  const std::string body = mangle_prefix_chain(qualified_name, true);
  return nested ? "N" + body : body;
}

std::string Mangler::mangle_template_type(const AbiType & type,
                                          std::size_t depth)
{
  if(type.kind == ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION) {
    const std::string key = key_of_type_impl(type, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    std::string result = "T";
    if(type.index != 0) {
      result += number_word(static_cast<unsigned long long>(type.index - 1));
    }
    result += "_";
    result += mangle_template_args(type.argument_refs);
    substitutions_.add(key);
    return result;
  }
  if(type.kind == ABI_TYPE_STD_TEMPLATE_SPECIALIZATION) {
    if(type.standard_substitution.empty()) {
      throw std::logic_error("standard template type needs a substitution code");
    }
    const std::string key = key_of_type_impl(type, depth + 1);
    std::string spelling;
    if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
    std::string result = type.standard_substitution;
    if(!type.standard_substitution_includes_arguments) {
      result += mangle_template_args(type.argument_refs);
    }
    substitutions_.add(key);
    return result;
  }
  const std::string key = key_of_type_impl(type, depth + 1);
  std::string spelling;
  if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
  const bool nested = nested_name(type.name);
  std::string result = mangle_template_name(type.name);
  result += mangle_template_args(type.argument_refs);
  if(nested) result += "E";
  substitutions_.add(key);
  return result;
}

std::string Mangler::mangle_template_member_name(const AbiType & type,
                                                 std::size_t depth)
{
  const std::string owner = mangle_owner_prefix(type.types.at(0), depth + 1);
  std::string result = "N" + owner + number_word(type.name.size()) + type.name;
  if(type.kind == ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION) {
    result += mangle_template_args(type.argument_refs);
  }
  return result + "E";
}

std::string Mangler::mangle_owner_prefix(const AbiType & input,
                                         std::size_t depth)
{
  check_depth(depth);
  if(input.kind == ABI_TYPE_NAME_OR_REFERENCE) {
    const AbiType * referenced = type_definition(input.name);
    if(referenced) return mangle_owner_prefix(*referenced, depth + 1);
    AbiType named = input;
    named.kind = ABI_TYPE_NAMED;
    return mangle_owner_prefix(named, depth + 1);
  }
  if(input.kind == ABI_TYPE_NAMED) {
    return mangle_type_body_without_outer_name(
      mangle_qualified_name(input.name, false, input.abi_tags));
  }
  if(input.kind == ABI_TYPE_TEMPLATE_SPECIALIZATION) {
    std::string prefix;
    if(!substitutions_.lookup(strip_scope(input.name), &prefix)) {
      prefix = mangle_template_name(input.name);
      if(!prefix.empty() && prefix[0] == 'N') {
        prefix.erase(0, 1);
      }
    }
    std::string result = prefix + mangle_template_args(input.argument_refs);
    return result;
  }
  if(input.kind == ABI_TYPE_STD_TEMPLATE_SPECIALIZATION) {
    if(input.standard_substitution.empty()) {
      throw std::logic_error("standard template owner needs a substitution code");
    }
    std::string result = input.standard_substitution;
    if(!input.standard_substitution_includes_arguments) {
      result += mangle_template_args(input.argument_refs);
    }
    return result;
  }
  const std::string full = mangle_type_impl(input, depth + 1);
  return mangle_type_body_without_outer_name(full);
}

std::string Mangler::mangle_member_type(const AbiType & type, std::size_t depth)
{
  const std::string key = key_of_type_impl(type, depth + 1);
  std::string spelling;
  if(lookup_substitution(&substitutions_, key, &spelling)) return spelling;
  if(type.kind == ABI_TYPE_MEMBER_POINTER) {
    const std::string result = "M" + mangle_type_impl(type.types.at(0), depth + 1) +
      mangle_type_impl(type.types.at(1), depth + 1);
    substitutions_.add(key);
    return result;
  }
  const std::string result = mangle_template_member_name(type, depth + 1);
  substitutions_.add(key);
  return result;
}

std::string Mangler::mangle_tag_list(const std::vector<std::string> & tags) const
{
  std::vector<std::string> sorted = tags;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string result;
  for(std::size_t i = 0; i < sorted.size(); ++i) {
    result += "B" + number_word(sorted[i].size()) + sorted[i];
  }
  return result;
}

std::string Mangler::mangle_integral_value(const AbiType & type,
                                           long long value) const
{
  const std::string code = builtin_code(type);
  const bool unsigned_type = code == "h" || code == "t" || code == "j" ||
    code == "m" || code == "y" || code == "o";
  unsigned int width = 64;
  if(code == "h") width = 8;
  if(code == "t") width = 16;
  if(code == "j") width = 32;
  if(unsigned_type) {
    unsigned long long bits = static_cast<unsigned long long>(value);
    if(width < 64) bits &= (1ULL << width) - 1;
    return "L" + code + number_word(bits) + "E";
  }
  if(value < 0) {
    const unsigned long long magnitude = 0ULL -
      static_cast<unsigned long long>(value);
    return "L" + code + "n" + number_word(magnitude) + "E";
  }
  return "L" + code + number_word(static_cast<unsigned long long>(value)) + "E";
}

std::string Mangler::mangle_template_args(const std::vector<std::string> & refs)
{
  std::string result = "I";
  for(std::size_t i = 0; i < refs.size(); ++i) {
    result += mangle_argument_ref(refs[i], 0);
  }
  return result + "E";
}

std::string Mangler::mangle_type_argument(const std::string & ref,
                                          std::size_t depth)
{
  const AbiType * type = type_definition(ref);
  if(!type) {
    throw std::logic_error("unknown ABI type definition '" + ref + "'");
  }
  return mangle_type_impl(*type, depth + 1);
}

std::string Mangler::mangle_argument_ref(const std::string & ref,
                                         std::size_t depth)
{
  const AbiTemplateArgument * argument = argument_definition(ref);
  if(!argument) {
    throw std::logic_error("unknown ABI template argument '" + ref + "'");
  }
  return mangle_template_arg(*argument);
}

std::string Mangler::mangle_template_arg(const AbiTemplateArgument & argument)
{
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_TYPE) {
    return mangle_type(argument.type);
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_VALUE) {
    return mangle_integral_value(argument.value_type, argument.value);
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE) {
    return "Tn" + mangle_type_argument(argument.expression_ref, 0) +
      mangle_integral_value(argument.value_type, argument.value);
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE) {
    std::string result = "T";
    if(argument.index != 0) {
      result += number_word(static_cast<unsigned long long>(argument.index - 1));
    }
    return result + "_";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY) {
    const std::string owner = mangle_owner_prefix(argument.type, 0);
    std::string result = "N" + owner;
    std::string substitution;
    if(substitutions_.lookup(argument.substitution, &substitution)) {
      result += substitution;
    } else {
      result += number_word(argument.name.size()) + argument.name;
      substitutions_.add(argument.substitution);
    }
    return result + "E";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY) {
    return "XadL" + argument.symbol + "EE";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_ENTITY) {
    const AbiEntityFact * entity = entity_definition(argument.entity_ref);
    if(!entity) {
      throw std::logic_error("unknown ABI entity '" + argument.entity_ref + "'");
    }
    return "XadL" + mangle_entity_encoding(*entity) + "EE";
  }
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY) {
    return mangle_qualified_name(argument.name, false);
  }
  throw std::logic_error("unsupported target");
}

std::string Mangler::mangle_expression(const AbiDependentExpression & expression)
{
  return mangle_expression_impl(expression, 0);
}

std::string Mangler::mangle_expression_impl(
  const AbiDependentExpression & expression, std::size_t depth)
{
  check_depth(depth);
  if(expression.kind == ABI_EXPRESSION_TEMPLATE_PARAMETER) {
    std::string result = "T";
    if(expression.index != 0) {
      result += number_word(static_cast<unsigned long long>(expression.index - 1));
    }
    return result + "_";
  }
  if(expression.kind == ABI_EXPRESSION_FUNCTION_PARAMETER) {
    return expression.index == 0 ? "fp_" : "fp" +
      number_word(static_cast<unsigned long long>(expression.index - 1)) + "_";
  }
  if(expression.kind == ABI_EXPRESSION_LITERAL ||
     expression.kind == ABI_EXPRESSION_INTEGRAL_VALUE) {
    return "Li" + number_word(static_cast<unsigned long long>(expression.value)) + "E";
  }
  throw std::logic_error("unsupported target");
}

std::string Mangler::mangle_entity_encoding(const AbiEntityFact & entity)
{
  Mangler nested(definitions_);
  return nested.mangle_entity_impl(entity, 0);
}

std::string Mangler::mangle_entity_impl(const AbiEntityFact & entity,
                                        std::size_t depth)
{
  check_depth(depth);
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return entity.qualified_name;
  }
  if(entity.kind == ABI_ENTITY_FACT_FUNCTION) {
    AbiFunctionShape shape;
    shape.target = entity.function;
    return mangle_function(shape);
  }
  const std::string name = entity.internal_linkage ?
    mangle_internal_name(entity.qualified_name) :
    mangle_qualified_name(entity.qualified_name, false);
  return "_Z" + name;
}

}  // namespace abi_mangle
