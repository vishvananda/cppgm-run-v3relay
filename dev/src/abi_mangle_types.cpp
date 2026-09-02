// Itanium <type>, <template-arg>, and <expression> encoding, the structural
// key authority, and the fixed fact vocabularies.

#include "abi_mangle_encoder.h"

#include <algorithm>
#include <stdexcept>

namespace abi_mangle {

// ---------------------------------------------------------------------------
// Vocabulary tables: one row per enum value, indexed by the reader (word ->
// enum), the serializer (enum -> word), and the encoder (enum -> code).

namespace {

const AbiBuiltinTypeInfo kBuiltinTypes[ABI_BUILTIN_COUNT] = {
  {"void", "v", 0, false},
  {"bool", "b", 0, false},
  {"char", "c", 8, false},
  {"schar", "a", 8, false},
  {"uchar", "h", 8, true},
  {"short", "s", 16, false},
  {"ushort", "t", 16, true},
  {"int", "i", 32, false},
  {"uint", "j", 32, true},
  {"long", "l", 64, false},
  {"ulong", "m", 64, true},
  {"longlong", "x", 64, false},
  {"ulonglong", "y", 64, true},
  {"int128", "n", 128, false},
  {"uint128", "o", 128, true},
  {"float", "f", 0, false},
  {"double", "d", 0, false},
  {"longdouble", "e", 0, false},
  {"float128", "g", 0, false},
  {"wchar", "w", 32, false},
  {"char16", "Ds", 16, true},
  {"char32", "Di", 32, true},
  {"nullptr", "Dn", 0, false},
  {"auto", "Da", 0, false},
  {"complex-float", "Cf", 0, false},
  {"complex-double", "Cd", 0, false},
  {"complex-longdouble", "Ce", 0, false},
};

const AbiSpecialFunctionInfo kSpecialFunctions[ABI_SPECIAL_FUNCTION_COUNT] = {
  {"constructor-complete", "C1"},
  {"constructor-base", "C2"},
  {"constructor-allocating", "C3"},
  {"destructor-deleting", "D0"},
  {"destructor-complete", "D1"},
  {"destructor-base", "D2"},
};

const AbiOperatorInfo kOperators[ABI_OPERATOR_COUNT] = {
  {"new", "nw", 0},
  {"new-array", "na", 0},
  {"delete", "dl", 0},
  {"delete-array", "da", 0},
  {"plus", "pl", "ps"},
  {"minus", "mi", "ng"},
  {"address-of", "an", "ad"},
  {"deref", "ml", "de"},
  {"unary-plus", "ps", 0},
  {"binary-plus", "pl", 0},
  {"unary-minus", "ng", 0},
  {"binary-minus", "mi", 0},
  {"bit-and", "an", 0},
  {"multiply", "ml", 0},
  {"divide", "dv", 0},
  {"remainder", "rm", 0},
  {"bit-or", "or", 0},
  {"bit-xor", "eo", 0},
  {"assign", "aS", 0},
  {"plus-assign", "pL", 0},
  {"minus-assign", "mI", 0},
  {"multiply-assign", "mL", 0},
  {"divide-assign", "dV", 0},
  {"remainder-assign", "rM", 0},
  {"bit-and-assign", "aN", 0},
  {"bit-or-assign", "oR", 0},
  {"bit-xor-assign", "eO", 0},
  {"shift-left", "ls", 0},
  {"shift-right", "rs", 0},
  {"shift-left-assign", "lS", 0},
  {"shift-right-assign", "rS", 0},
  {"equal", "eq", 0},
  {"not-equal", "ne", 0},
  {"less", "lt", 0},
  {"greater", "gt", 0},
  {"less-equal", "le", 0},
  {"greater-equal", "ge", 0},
  {"spaceship", "ss", 0},
  {"logical-not", "nt", 0},
  {"logical-and", "aa", 0},
  {"logical-or", "oo", 0},
  {"complement", "co", 0},
  {"increment", "pp", 0},
  {"decrement", "mm", 0},
  {"comma", "cm", 0},
  {"member-pointer", "pm", 0},
  {"arrow", "pt", 0},
  {"call", "cl", 0},
  {"index", "ix", 0},
  {"co-await", "aw", 0},
};

// Accepted alternate spellings of operator words.
struct OperatorAlias
{
  const char * word;
  AbiOperatorKind kind;
};

const OperatorAlias kOperatorAliases[] = {
  {"dereference", ABI_OPERATOR_DEREF},
  {"modulo", ABI_OPERATOR_REMAINDER},
  {"modulo-assign", ABI_OPERATOR_REMAINDER_ASSIGN},
  {"subscript", ABI_OPERATOR_INDEX},
  {"arrow-star", ABI_OPERATOR_MEMBER_POINTER},
};

std::string number_word(unsigned long long value)
{
  return std::to_string(value);
}

std::string signed_number_word(long long value)
{
  if(value >= 0) {
    return number_word(static_cast<unsigned long long>(value));
  }
  return "n" + number_word(0ULL - static_cast<unsigned long long>(value));
}

std::string id_word(AbiKeyId id)
{
  return number_word(static_cast<unsigned long long>(id));
}

std::string source_name(const std::string & name)
{
  return number_word(name.size()) + name;
}

std::string strip_scope(const std::string & name)
{
  return name.compare(0, 2, "::") == 0 ? name.substr(2) : name;
}

std::string template_param_spelling(std::size_t index)
{
  return index == 0 ? std::string("T_") :
    "T" + number_word(static_cast<unsigned long long>(index - 1)) + "_";
}

std::string expression_operator_code(const std::string & operation)
{
  if(operation == "static-cast") return "sc";
  if(operation == "const-cast") return "cc";
  if(operation == "reinterpret-cast") return "rc";
  if(operation == "dynamic-cast") return "dc";
  if(operation == "c-style-cast") return "cv";
  if(operation == "conditional") return "qu";
  if(operation == "pack-expansion") return "sp";
  return operation;
}

std::string canonical_tags(const std::vector<std::string> & tags)
{
  std::vector<std::string> sorted = tags;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string key;
  for(std::size_t i = 0; i < sorted.size(); ++i) {
    key += "B" + source_name(sorted[i]);
  }
  return key;
}

// Decimal spelling of a 128-bit unsigned value.
std::string wide_number_word(unsigned __int128 value)
{
  if(value == 0) {
    return "0";
  }
  std::string digits;
  while(value != 0) {
    digits.insert(digits.begin(), static_cast<char>('0' + value % 10));
    value /= 10;
  }
  return digits;
}

}  // namespace

const AbiBuiltinTypeInfo & builtin_type_info(AbiBuiltinType type)
{
  return kBuiltinTypes[type];
}

bool lookup_builtin_type(const std::string & word, AbiBuiltinType * type)
{
  for(std::size_t i = 0; i < ABI_BUILTIN_COUNT; ++i) {
    if(word == kBuiltinTypes[i].word) {
      *type = static_cast<AbiBuiltinType>(i);
      return true;
    }
  }
  return false;
}

const AbiSpecialFunctionInfo & special_function_info(AbiSpecialFunctionKind kind)
{
  return kSpecialFunctions[kind];
}

bool lookup_special_function(const std::string & word,
                             AbiSpecialFunctionKind * kind)
{
  for(std::size_t i = 0; i < ABI_SPECIAL_FUNCTION_COUNT; ++i) {
    if(word == kSpecialFunctions[i].word) {
      *kind = static_cast<AbiSpecialFunctionKind>(i);
      return true;
    }
  }
  return false;
}

const AbiOperatorInfo & operator_info(AbiOperatorKind kind)
{
  return kOperators[kind];
}

bool lookup_operator(const std::string & word, AbiOperatorKind * kind)
{
  for(std::size_t i = 0; i < ABI_OPERATOR_COUNT; ++i) {
    if(word == kOperators[i].word) {
      *kind = static_cast<AbiOperatorKind>(i);
      return true;
    }
  }
  const std::size_t aliases = sizeof(kOperatorAliases) / sizeof(kOperatorAliases[0]);
  for(std::size_t i = 0; i < aliases; ++i) {
    if(word == kOperatorAliases[i].word) {
      *kind = kOperatorAliases[i].kind;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Encoder state

AbiKeyId KeyInterner::intern(const std::string & spelling)
{
  const std::unordered_map<std::string, AbiKeyId>::const_iterator it =
    ids_.find(spelling);
  if(it != ids_.end()) {
    return it->second;
  }
  const AbiKeyId id = ids_.size() + 1;
  ids_.insert(std::make_pair(spelling, id));
  return id;
}

bool SubstitutionTable::lookup(AbiKeyId key, std::string * spelling) const
{
  if(key == 0 || key >= slots_.size() || slots_[key] == 0) {
    return false;
  }
  std::size_t index = slots_[key] - 1;
  if(index == 0) {
    *spelling = "S_";
    return true;
  }
  --index;
  std::string digits;
  do {
    const std::size_t digit = index % 36;
    digits.insert(digits.begin(),
      static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10));
    index /= 36;
  } while(index != 0);
  *spelling = "S" + digits + "_";
  return true;
}

void SubstitutionTable::add(AbiKeyId key)
{
  if(key == 0) {
    return;
  }
  if(key >= slots_.size()) {
    slots_.resize(key + 1, 0);
  }
  if(slots_[key] == 0) {
    slots_[key] = ++count_;
  }
}

DepthScope::DepthScope(Mangler * mangler)
  : mangler_(mangler)
{
  if(++mangler_->depth_ > ABI_MAXIMUM_NESTING_DEPTH) {
    --mangler_->depth_;
    throw std::logic_error("ABI facts nest too deeply");
  }
}

DepthScope::~DepthScope()
{
  --mangler_->depth_;
}

Mangler::Mangler(const AbiDefinitionTable & definitions, std::size_t depth)
  : definitions_(definitions), depth_(depth)
{
}

const AbiType & Mangler::type_definition(const std::string & ref) const
{
  const AbiType * type = definitions_.find_type(ref);
  if(!type) {
    throw std::logic_error("unknown ABI type definition '" + ref + "'");
  }
  return *type;
}

const AbiTemplateArgument & Mangler::argument_definition(
  const std::string & ref) const
{
  const AbiTemplateArgument * argument = definitions_.find_argument(ref);
  if(!argument) {
    throw std::logic_error("unknown ABI template argument '" + ref + "'");
  }
  return *argument;
}

const AbiDependentExpression & Mangler::expression_definition(
  const std::string & ref) const
{
  const AbiDependentExpression * expression = definitions_.find_expression(ref);
  if(!expression) {
    throw std::logic_error("unknown ABI expression '" + ref + "'");
  }
  return *expression;
}

const AbiEntityFact & Mangler::entity_definition(const std::string & ref) const
{
  const AbiEntityFact * entity = definitions_.find_entity(ref);
  if(!entity) {
    throw std::logic_error("unknown ABI entity '" + ref + "'");
  }
  return *entity;
}

const AbiLocalContext & Mangler::context_definition(const std::string & ref) const
{
  const AbiLocalContext * context = definitions_.find_context(ref);
  if(!context) {
    throw std::logic_error("unknown ABI context '" + ref + "'");
  }
  return *context;
}

// Follows definition aliases (a NAME_OR_REFERENCE naming a let-type) to the
// node that carries structure, accumulating cv qualifiers.  The walk is
// bounded by the number of definitions, so alias cycles are rejected without
// recursion.
ResolvedType Mangler::resolve_type(const AbiType & input) const
{
  ResolvedType resolved;
  resolved.node = &input;
  std::size_t hops = 0;
  while(true) {
    resolved.is_const = resolved.is_const || resolved.node->is_const;
    resolved.is_volatile = resolved.is_volatile || resolved.node->is_volatile;
    if(resolved.node->kind != ABI_TYPE_NAME_OR_REFERENCE) {
      return resolved;
    }
    const AbiType * definition = definitions_.find_type(resolved.node->name);
    if(!definition) {
      return resolved;
    }
    if(!resolved.node->abi_tags.empty()) {
      throw std::logic_error("ABI tags must be attached to a named or template "
        "type, not to the definition '" + resolved.node->name + "'");
    }
    if(++hops > definitions_.size()) {
      throw std::logic_error("cyclic ABI type definition '" +
        resolved.node->name + "'");
    }
    resolved.node = definition;
  }
}

// ---------------------------------------------------------------------------
// Structural keys

AbiKeyId Mangler::cached_key(const void * fact, bool * ready)
{
  std::unordered_map<const void *, KeyCacheEntry>::iterator it =
    key_cache_.find(fact);
  if(it == key_cache_.end()) {
    KeyCacheEntry entry;
    entry.in_progress = true;
    key_cache_.insert(std::make_pair(fact, entry));
    *ready = false;
    return 0;
  }
  if(it->second.in_progress) {
    throw std::logic_error("cyclic ABI definition");
  }
  *ready = true;
  return it->second.key;
}

void Mangler::store_key(const void * fact, AbiKeyId key)
{
  KeyCacheEntry & entry = key_cache_[fact];
  entry.key = key;
  entry.in_progress = false;
}

void Mangler::release_key(const void * fact)
{
  key_cache_.erase(fact);
}

AbiKeyId Mangler::key_of_type(const AbiType & input, bool ignore_cv)
{
  const ResolvedType resolved = resolve_type(input);
  const AbiKeyId base = key_of_node(*resolved.node);
  if(ignore_cv || !(resolved.is_const || resolved.is_volatile)) {
    return base;
  }
  std::string spelling;
  if(resolved.is_volatile) spelling += "V";
  if(resolved.is_const) spelling += "K";
  return keys_.intern(spelling + "(" + id_word(base) + ")");
}

// Every node the encoder sees is owned by the case for the whole name, so
// its key is memoized by address; the in-progress state rejects definition
// cycles.
AbiKeyId Mangler::key_of_node(const AbiType & node)
{
  bool ready = false;
  const AbiKeyId cached = cached_key(&node, &ready);
  if(ready) {
    return cached;
  }
  try {
    const AbiKeyId key = key_of_node_impl(node);
    store_key(&node, key);
    return key;
  } catch(...) {
    release_key(&node);
    throw;
  }
}

// Key of a type node with its own cv qualifiers ignored (the caller layers
// them).  Children are referenced by id, so the spelling is bounded by the
// node's fan-out.
AbiKeyId Mangler::key_of_node_impl(const AbiType & node)
{
  DepthScope scope(this);
  switch(node.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
  case ABI_TYPE_NAMED:
    return name_key(node.name, node.abi_tags);
  case ABI_TYPE_BUILTIN:
    return keys_.intern(std::string("b:") + builtin_type_info(node.builtin).code);
  case ABI_TYPE_TEMPLATE_PARAMETER:
    return keys_.intern("T" + number_word(node.index));
  case ABI_TYPE_POINTER:
    return keys_.intern("P(" + id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_LVALUE_REFERENCE:
    return keys_.intern("R(" + id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_RVALUE_REFERENCE:
    return keys_.intern("O(" + id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_PACK_EXPANSION:
    return keys_.intern("Dp(" + id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_VENDOR_QUALIFIED:
    return keys_.intern("U" + source_name(node.name) + "(" +
      id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_ARRAY:
    return keys_.intern("A" + node.array_bound.value + "(" +
      id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_BUILTIN_TRANSFORM:
    return keys_.intern("u" + source_name(node.name) + "(" +
      id_word(key_of_type(node.types.at(0), false)) + ")");
  case ABI_TYPE_FUNCTION: {
    std::string spelling = "F(";
    for(std::size_t i = 0; i < node.types.size(); ++i) {
      spelling += id_word(key_of_type(node.types[i], false)) + ";";
    }
    return keys_.intern(spelling + (node.variadic ? "z)" : ")"));
  }
  case ABI_TYPE_MEMBER_POINTER:
    return keys_.intern("M(" + id_word(key_of_type(node.types.at(0), false)) +
      "," + id_word(key_of_type(node.types.at(1), false)) + ")");
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION: {
    if(node.kind == ABI_TYPE_STD_TEMPLATE_SPECIALIZATION &&
       node.standard_substitution_includes_arguments) {
      return keys_.intern("std(" + node.standard_substitution + ")");
    }
    std::string spelling = node.kind == ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION ?
      "TT(" + number_word(node.index) :
      "S(" + id_word(name_key(node.name, node.abi_tags));
    for(std::size_t i = 0; i < node.argument_refs.size(); ++i) {
      spelling += ":" + id_word(key_of_argument_ref(node.argument_refs[i]));
    }
    return keys_.intern(spelling + ")");
  }
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION: {
    std::string spelling = "m(" + id_word(key_of_type(node.types.at(0), true)) +
      ":" + node.name;
    for(std::size_t i = 0; i < node.argument_refs.size(); ++i) {
      spelling += ":" + id_word(key_of_argument_ref(node.argument_refs[i]));
    }
    return keys_.intern(spelling + ")");
  }
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    return keys_.intern("DT(" + id_word(key_of_expression_ref(node.expression_ref)) +
      ")");
  case ABI_TYPE_LAMBDA_CLOSURE:
    return lambda_closure_key(node.context_ref, node.discriminator, node.types);
  case ABI_TYPE_LOCAL_TYPE:
    return local_type_key(node.context_ref, node.name, node.discriminator);
  case ABI_TYPE_NAMESPACE_LAMBDA:
    return namespace_lambda_key(node.namespace_qualifiers, node.name);
  }
  throw std::logic_error("unknown ABI type kind");
}

// Local classes, closures, and namespace-scope closures are keyed the same
// way whether they appear as types or as the owner of a call operator.

AbiKeyId Mangler::local_type_key(const std::string & context_ref,
                                 const std::string & name,
                                 const std::string & discriminator)
{
  const bool first = discriminator.empty() || discriminator == "-" ||
    discriminator == "0";
  return keys_.intern("Z(" + context_ref + ":" + name + ":" +
    (first ? std::string("0") : discriminator) + ")");
}

AbiKeyId Mangler::lambda_closure_key(const std::string & context_ref,
                                     const std::string & discriminator,
                                     const std::vector<AbiType> & signature)
{
  std::string spelling = "Ul(" + context_ref + ":" +
    (discriminator == "-" ? std::string() : discriminator);
  for(std::size_t i = 0; i < signature.size(); ++i) {
    spelling += ":" + id_word(key_of_type(signature[i], false));
  }
  return keys_.intern(spelling + ")");
}

AbiKeyId Mangler::namespace_lambda_key(const std::vector<std::string> & qualifiers,
                                       const std::string & name)
{
  std::string spelling = "NL(";
  for(std::size_t i = 0; i < qualifiers.size(); ++i) {
    spelling += qualifiers[i] + "::";
  }
  return keys_.intern(spelling + name + ")");
}

AbiKeyId Mangler::key_of_argument_ref(const std::string & ref)
{
  const AbiTemplateArgument & argument = argument_definition(ref);
  bool ready = false;
  const AbiKeyId cached = cached_key(&argument, &ready);
  if(ready) {
    return cached;
  }
  try {
    const AbiKeyId key = key_of_argument(argument);
    store_key(&argument, key);
    return key;
  } catch(...) {
    release_key(&argument);
    throw;
  }
}

AbiKeyId Mangler::key_of_argument(const AbiTemplateArgument & argument)
{
  DepthScope scope(this);
  switch(argument.kind) {
  case ABI_TEMPLATE_ARGUMENT_TYPE:
    return key_of_type(argument.type, false);
  case ABI_TEMPLATE_ARGUMENT_VALUE:
    return keys_.intern("v(" + mangle_integral_value(argument.value_type,
      argument.value) + ")");
  case ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE:
    return keys_.intern("dv(" + id_word(key_of_type(argument.type, false)) + ":" +
      mangle_integral_value(argument.value_type, argument.value) + ")");
  case ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE:
    return keys_.intern("uv(" + signed_number_word(argument.value) + ")");
  case ABI_TEMPLATE_ARGUMENT_EXPRESSION:
    return keys_.intern("ax(" + id_word(key_of_expression_ref(
      argument.expression_ref)) + ")");
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY:
    return keys_.intern("at(" + id_word(name_key(argument.name,
      std::vector<std::string>())) + ")");
  case ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY:
    return keys_.intern("am(" + id_word(key_of_type(argument.type, true)) + ":" +
      argument.name + ")");
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE:
    return keys_.intern("ap(" + number_word(argument.index) + ")");
  case ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY:
    return keys_.intern("ae(" + argument.symbol + ")");
  case ABI_TEMPLATE_ARGUMENT_ENTITY:
    return keys_.intern(std::string(argument.address_of ? "aad(" : "aen(") +
      id_word(key_of_entity(entity_definition(argument.entity_ref))) + ")");
  case ABI_TEMPLATE_ARGUMENT_PACK: {
    std::string spelling = "J(";
    for(std::size_t i = 0; i < argument.argument_refs.size(); ++i) {
      spelling += id_word(key_of_argument_ref(argument.argument_refs[i])) + ";";
    }
    return keys_.intern(spelling + ")");
  }
  }
  throw std::logic_error("unknown ABI template argument kind");
}

AbiKeyId Mangler::key_of_expression_ref(const std::string & ref)
{
  const AbiDependentExpression & expression = expression_definition(ref);
  bool ready = false;
  const AbiKeyId cached = cached_key(&expression, &ready);
  if(ready) {
    return cached;
  }
  try {
    const AbiKeyId key = key_of_expression(expression);
    store_key(&expression, key);
    return key;
  } catch(...) {
    release_key(&expression);
    throw;
  }
}

AbiKeyId Mangler::key_of_expression(const AbiDependentExpression & expression)
{
  DepthScope scope(this);
  std::string spelling = "x" + number_word(expression.kind) + "(";
  switch(expression.kind) {
  case ABI_EXPRESSION_TEMPLATE_PARAMETER:
  case ABI_EXPRESSION_FUNCTION_PARAMETER:
    spelling += number_word(expression.index);
    break;
  case ABI_EXPRESSION_LITERAL:
  case ABI_EXPRESSION_INTEGRAL_VALUE:
    spelling += signed_number_word(expression.value);
    break;
  case ABI_EXPRESSION_UNARY:
  case ABI_EXPRESSION_BINARY:
  case ABI_EXPRESSION_OBJECT_MEMBER:
    spelling += expression_operator_code(expression.op) + ":" + expression.text;
    break;
  case ABI_EXPRESSION_CONVERSION:
  case ABI_EXPRESSION_CAST:
    spelling += expression_operator_code(expression.op) + ":" +
      id_word(key_of_type(expression.type, false));
    break;
  case ABI_EXPRESSION_SIZEOF_TYPE:
    spelling += id_word(key_of_type(expression.type, false));
    break;
  case ABI_EXPRESSION_MEMBER:
    spelling += id_word(key_of_type(expression.type, false)) + ":" +
      (expression.close_member_owner ? "E:" : ":") + expression.text;
    break;
  case ABI_EXPRESSION_TEMPLATE_ID:
  case ABI_EXPRESSION_TYPE_TRAIT:
    spelling += expression.text;
    break;
  case ABI_EXPRESSION_EXTERNAL_ENTITY:
  case ABI_EXPRESSION_ENTITY:
    spelling += id_word(key_of_entity(entity_definition(expression.entity_ref)));
    break;
  case ABI_EXPRESSION_CONDITIONAL:
  case ABI_EXPRESSION_PACK_EXPANSION:
  case ABI_EXPRESSION_CALL:
    break;
  }
  for(std::size_t i = 0; i < expression.expression_refs.size(); ++i) {
    spelling += ";" + id_word(key_of_expression_ref(expression.expression_refs[i]));
  }
  for(std::size_t i = 0; i < expression.argument_refs.size(); ++i) {
    spelling += ",a" + id_word(key_of_argument_ref(expression.argument_refs[i]));
  }
  for(std::size_t i = 0; i < expression.type_arguments.size(); ++i) {
    spelling += ",t" + id_word(key_of_type(expression.type_arguments[i], false));
  }
  return keys_.intern(spelling + ")");
}

AbiKeyId Mangler::key_of_entity(const AbiEntityFact & entity)
{
  DepthScope scope(this);
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return keys_.intern("sym(" + entity.qualified_name + ")");
  }
  if(entity.kind == ABI_ENTITY_FACT_VARIABLE) {
    return keys_.intern(std::string(entity.internal_linkage ? "ivar(" : "var(") +
      id_word(name_key(entity.qualified_name, std::vector<std::string>())) + ")");
  }
  std::string spelling = "fn(" + id_word(name_key(entity.function.qualified_name,
    std::vector<std::string>()));
  for(std::size_t i = 0; i < entity.function.path_operands.size(); ++i) {
    const AbiFunctionPathOperand & operand = entity.function.path_operands[i];
    if(operand.kind == ABI_FUNCTION_PATH_TYPE) {
      spelling += ";t" + id_word(key_of_type(operand.type, false));
    } else if(operand.kind == ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT) {
      spelling += ";a" + id_word(key_of_argument_ref(operand.argument_ref));
    } else {
      spelling += ";z";
    }
  }
  return keys_.intern(spelling + ")");
}

// ---------------------------------------------------------------------------
// <type>

std::string Mangler::mangle_type(const AbiType & type)
{
  return mangle_type_impl(type, false);
}

std::string Mangler::mangle_type_impl(const AbiType & input, bool ignore_cv)
{
  DepthScope scope(this);
  const ResolvedType resolved = resolve_type(input);
  const bool qualified = !ignore_cv && (resolved.is_const || resolved.is_volatile);
  if(!qualified) {
    return mangle_node(*resolved.node, resolved.node->name);
  }
  const AbiKeyId key = key_of_type(input, false);
  std::string spelling;
  if(substitutions_.lookup(key, &spelling)) {
    return spelling;
  }
  std::string result;
  if(resolved.is_volatile) result += "V";
  if(resolved.is_const) result += "K";
  result += mangle_node(*resolved.node, resolved.node->name);
  substitutions_.add(key);
  return result;
}

// Emits one resolved node, ignoring its own cv qualifiers.  Every candidate
// kind looks its key up first and registers it after emission, children
// before parents, which is the Itanium substitution order.
std::string Mangler::mangle_node(const AbiType & node,
                                 const std::string & class_name)
{
  switch(node.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
    return mangle_named_type(class_name, node.abi_tags);
  case ABI_TYPE_NAMED:
    return mangle_named_type(node.name, node.abi_tags);
  case ABI_TYPE_BUILTIN:
    return builtin_type_info(node.builtin).code;
  case ABI_TYPE_TEMPLATE_PARAMETER: {
    if(!node.substitutable) {
      return template_param_spelling(node.index);
    }
    const AbiKeyId key = key_of_node(node);
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) return spelling;
    substitutions_.add(key);
    return template_param_spelling(node.index);
  }
  case ABI_TYPE_POINTER:
  case ABI_TYPE_LVALUE_REFERENCE:
  case ABI_TYPE_RVALUE_REFERENCE:
  case ABI_TYPE_PACK_EXPANSION:
  case ABI_TYPE_VENDOR_QUALIFIED:
  case ABI_TYPE_ARRAY:
  case ABI_TYPE_BUILTIN_TRANSFORM:
  case ABI_TYPE_FUNCTION:
  case ABI_TYPE_MEMBER_POINTER: {
    const AbiKeyId key = key_of_node(node);
    std::string result;
    if(substitutions_.lookup(key, &result)) return result;
    if(node.kind == ABI_TYPE_POINTER) {
      result = "P" + mangle_type_impl(node.types.at(0), false);
    } else if(node.kind == ABI_TYPE_LVALUE_REFERENCE) {
      result = "R" + mangle_type_impl(node.types.at(0), false);
    } else if(node.kind == ABI_TYPE_RVALUE_REFERENCE) {
      result = "O" + mangle_type_impl(node.types.at(0), false);
    } else if(node.kind == ABI_TYPE_PACK_EXPANSION) {
      result = "Dp" + mangle_type_impl(node.types.at(0), false);
    } else if(node.kind == ABI_TYPE_VENDOR_QUALIFIED) {
      result = "U" + source_name(node.name) +
        mangle_type_impl(node.types.at(0), false);
    } else if(node.kind == ABI_TYPE_ARRAY) {
      if(node.array_bound.kind != ABI_ARRAY_BOUND_VALUE) {
        throw std::logic_error("unsupported non-value array bound");
      }
      result = "A" + node.array_bound.value + "_" +
        mangle_type_impl(node.types.at(0), false);
    } else if(node.kind == ABI_TYPE_BUILTIN_TRANSFORM) {
      result = "u" + source_name(node.name) + "I" +
        mangle_type_impl(node.types.at(0), false) + "E";
    } else if(node.kind == ABI_TYPE_FUNCTION) {
      if(node.types.empty()) {
        throw std::logic_error("function ABI type needs a result");
      }
      result = "F" + mangle_type_impl(node.types[0], false);
      if(node.types.size() == 1) {
        result += "v";
      }
      for(std::size_t i = 1; i < node.types.size(); ++i) {
        result += mangle_type_impl(node.types[i], false);
      }
      if(node.variadic) result += "z";
      result += "E";
    } else {
      result = "M" + mangle_type_impl(node.types.at(0), false) +
        mangle_type_impl(node.types.at(1), false);
    }
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION: {
    const AbiKeyId key = key_of_node(node);
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) return spelling;
    bool nested = false;
    const std::string body = mangle_template_body(node, &nested);
    return nested ? "N" + body + "E" : body;
  }
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION:
    return mangle_member_type(node);
  case ABI_TYPE_DECLTYPE_EXPRESSION: {
    const AbiKeyId key = key_of_node(node);
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) return spelling;
    const std::string result = "DT" +
      mangle_expression_impl(expression_definition(node.expression_ref)) + "E";
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_LAMBDA_CLOSURE:
  case ABI_TYPE_LOCAL_TYPE: {
    const AbiKeyId key = key_of_node(node);
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) return spelling;
    std::string context;
    const std::string body = mangle_local_body(node, &context);
    substitutions_.add(key);
    return context + body;
  }
  case ABI_TYPE_NAMESPACE_LAMBDA: {
    const AbiKeyId key = key_of_node(node);
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) return spelling;
    std::string result = "N";
    append_prefixes(node.namespace_qualifiers, node.namespace_qualifiers.size(),
                    &result);
    result += source_name(node.name) + "E";
    substitutions_.add(key);
    return result;
  }
  }
  throw std::logic_error("unknown ABI type kind");
}

std::string Mangler::mangle_named_type(const std::string & qualified_name,
                                       const std::vector<std::string> & tags)
{
  const AbiKeyId key = name_key(qualified_name, tags);
  std::string spelling;
  if(substitutions_.lookup(key, &spelling)) {
    return spelling;
  }
  bool nested = false;
  const std::string body = mangle_name_body(qualified_name, tags, key, &nested);
  return nested ? "N" + body + "E" : body;
}

// Body of a template specialization without the N...E wrapper; registers the
// template name and then the specialization.  The caller has already checked
// the specialization for substitution.
std::string Mangler::mangle_template_body(const AbiType & node, bool * nested)
{
  const AbiKeyId key = key_of_node(node);
  *nested = false;
  std::string result;
  if(node.kind == ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION) {
    result = template_param_spelling(node.index);
    result += mangle_template_args(node.argument_refs);
    substitutions_.add(key);
    return result;
  }
  if(node.kind == ABI_TYPE_STD_TEMPLATE_SPECIALIZATION) {
    if(node.standard_substitution.empty()) {
      throw std::logic_error("standard template type needs a substitution code");
    }
    result = node.standard_substitution;
    if(!node.standard_substitution_includes_arguments) {
      result += mangle_template_args(node.argument_refs);
      substitutions_.add(key);
    }
    return result;
  }
  const AbiKeyId template_key = name_key(node.name, node.abi_tags);
  *nested = needs_nested_name(name_components(node.name));
  if(!substitutions_.lookup(template_key, &result)) {
    result = mangle_name_body(node.name, node.abi_tags, template_key, nested);
  }
  result += mangle_template_args(node.argument_refs);
  substitutions_.add(key);
  return result;
}

// <nested-name> of a member type: the owner is a <prefix> candidate, so it is
// looked up and registered as the same component as the owner type itself.
std::string Mangler::mangle_member_type(const AbiType & node)
{
  const AbiKeyId key = key_of_node(node);
  std::string spelling;
  if(substitutions_.lookup(key, &spelling)) return spelling;
  std::string context;
  std::string body = mangle_owner_body(node.types.at(0), &context);
  body += source_name(node.name);
  if(node.kind == ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION) {
    body += mangle_template_args(node.argument_refs);
  }
  substitutions_.add(key);
  return context + "N" + body + "E";
}

// The <prefix> spelling of an owner type without its own N...E wrapper.  A
// local owner contributes its Z...E context, which wraps the whole nested
// name and is returned separately.
std::string Mangler::mangle_owner_body(const AbiType & input,
                                       std::string * context)
{
  DepthScope scope(this);
  const ResolvedType resolved = resolve_type(input);
  const AbiType & node = *resolved.node;
  const AbiKeyId key = key_of_node(node);
  std::string result;
  switch(node.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
  case ABI_TYPE_NAMED: {
    if(substitutions_.lookup(key, &result)) return result;
    bool nested = false;
    return mangle_name_body(node.name, node.abi_tags, key, &nested);
  }
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION: {
    if(substitutions_.lookup(key, &result)) return result;
    bool nested = false;
    return mangle_template_body(node, &nested);
  }
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION: {
    if(substitutions_.lookup(key, &result)) return result;
    result = mangle_owner_body(node.types.at(0), context) + source_name(node.name);
    if(node.kind == ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION) {
      result += mangle_template_args(node.argument_refs);
    }
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_LAMBDA_CLOSURE:
  case ABI_TYPE_LOCAL_TYPE: {
    if(substitutions_.lookup(key, &result)) return result;
    result = mangle_local_body(node, context);
    substitutions_.add(key);
    return result;
  }
  case ABI_TYPE_NAMESPACE_LAMBDA: {
    if(substitutions_.lookup(key, &result)) return result;
    append_prefixes(node.namespace_qualifiers, node.namespace_qualifiers.size(),
                    &result);
    result += source_name(node.name);
    substitutions_.add(key);
    return result;
  }
  default:
    return mangle_node(node, node.name);
  }
}

// <local-name> parts of a local class or closure type: the Z...E context
// and the entity name that follows it.
std::string Mangler::mangle_local_body(const AbiType & node,
                                       std::string * context)
{
  *context = mangle_context(node.context_ref);
  if(node.kind == ABI_TYPE_LOCAL_TYPE) {
    std::string result = source_name(node.name);
    if(!node.discriminator.empty() && node.discriminator != "-") {
      const unsigned long long value = std::stoull(node.discriminator);
      if(value != 0) result += "_" + number_word(value - 1);
    }
    return result;
  }
  std::string result = "Ul";
  if(node.types.empty()) {
    result += "v";
  }
  for(std::size_t i = 0; i < node.types.size(); ++i) {
    result += mangle_type_impl(node.types[i], false);
  }
  result += "E";
  if(!node.discriminator.empty() && node.discriminator != "-") {
    result += node.discriminator;
  }
  return result + "_";
}

// ---------------------------------------------------------------------------
// <template-args> and <template-arg>

std::string Mangler::mangle_template_args(const std::vector<std::string> & refs)
{
  std::string result = "I";
  for(std::size_t i = 0; i < refs.size(); ++i) {
    result += mangle_argument_ref(refs[i]);
  }
  return result + "E";
}

std::string Mangler::mangle_argument_ref(const std::string & ref)
{
  return mangle_template_arg(argument_definition(ref));
}

std::string Mangler::mangle_integral_value(const AbiType & type,
                                           long long value) const
{
  const ResolvedType resolved = resolve_type(type);
  if(resolved.node->kind != ABI_TYPE_BUILTIN) {
    throw std::logic_error("integral ABI value needs a builtin type");
  }
  const AbiBuiltinTypeInfo & info = builtin_type_info(resolved.node->builtin);
  std::string result = "L";
  result += info.code;
  if(info.is_unsigned && value < 0) {
    // A negative stored value for an unsigned type is its modulo bit pattern.
    unsigned __int128 bits = static_cast<unsigned __int128>(
      static_cast<__int128>(value));
    if(info.bits < 128) {
      bits &= (static_cast<unsigned __int128>(1) << info.bits) - 1;
    }
    result += wide_number_word(bits);
  } else {
    result += signed_number_word(value);
  }
  return result + "E";
}

std::string Mangler::mangle_template_arg(const AbiTemplateArgument & argument)
{
  DepthScope scope(this);
  switch(argument.kind) {
  case ABI_TEMPLATE_ARGUMENT_TYPE:
    return mangle_type_impl(argument.type, false);
  case ABI_TEMPLATE_ARGUMENT_VALUE:
    return mangle_integral_value(argument.value_type, argument.value);
  case ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE:
    return "Tn" + mangle_type_impl(argument.type, false) +
      mangle_integral_value(argument.value_type, argument.value);
  case ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE:
    return "L" + signed_number_word(argument.value) + "E";
  case ABI_TEMPLATE_ARGUMENT_EXPRESSION:
    return "X" + mangle_expression_impl(expression_definition(
      argument.expression_ref)) + "E";
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE:
    return template_param_spelling(argument.index);
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY:
    return mangle_qualified_name(argument.name);
  case ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY: {
    // The owner specialization is not registered here; the fixtures pin the
    // member template as the next slot.
    const ResolvedType owner = resolve_type(argument.type);
    bool nested = false;
    std::string result = "N";
    if(owner.node->kind == ABI_TYPE_TEMPLATE_SPECIALIZATION) {
      const AbiKeyId template_key = name_key(owner.node->name,
                                             owner.node->abi_tags);
      std::string prefix;
      if(!substitutions_.lookup(template_key, &prefix)) {
        prefix = mangle_name_body(owner.node->name, owner.node->abi_tags,
                                  template_key, &nested);
      }
      result += prefix + mangle_template_args(owner.node->argument_refs);
    } else {
      std::string context;
      result += mangle_owner_body(argument.type, &context);
      result = context + result;
    }
    const AbiKeyId member_key = name_key(argument.substitution,
                                         std::vector<std::string>());
    std::string spelling;
    if(substitutions_.lookup(member_key, &spelling)) {
      result += spelling;
    } else {
      result += source_name(argument.name);
      substitutions_.add(member_key);
    }
    return result + "E";
  }
  case ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY:
    return "XadL" + argument.symbol + "EE";
  case ABI_TEMPLATE_ARGUMENT_ENTITY:
    return std::string(argument.address_of ? "XadL" : "XL") +
      mangle_entity_encoding(entity_definition(argument.entity_ref)) + "EE";
  case ABI_TEMPLATE_ARGUMENT_PACK: {
    std::string result = "J";
    for(std::size_t i = 0; i < argument.argument_refs.size(); ++i) {
      result += mangle_argument_ref(argument.argument_refs[i]);
    }
    return result + "E";
  }
  }
  throw std::logic_error("unknown ABI template argument kind");
}

// ---------------------------------------------------------------------------
// <expression>

std::string Mangler::mangle_expression(const AbiDependentExpression & expression)
{
  return mangle_expression_impl(expression);
}

std::string Mangler::mangle_expression_ref(const std::string & ref)
{
  return mangle_expression_impl(expression_definition(ref));
}

std::string Mangler::mangle_expression_impl(
  const AbiDependentExpression & expression)
{
  DepthScope scope(this);
  switch(expression.kind) {
  case ABI_EXPRESSION_TEMPLATE_PARAMETER:
    return template_param_spelling(expression.index);
  case ABI_EXPRESSION_FUNCTION_PARAMETER:
    return expression.index == 0 ? std::string("fp_") :
      "fp" + number_word(expression.index - 1) + "_";
  case ABI_EXPRESSION_LITERAL:
  case ABI_EXPRESSION_INTEGRAL_VALUE:
    return "Li" + signed_number_word(expression.value) + "E";
  case ABI_EXPRESSION_UNARY:
    return expression_operator_code(expression.op) +
      mangle_expression_ref(expression.expression_refs.at(0));
  case ABI_EXPRESSION_BINARY: {
    const std::string left = mangle_expression_ref(expression.expression_refs.at(0));
    const std::string right = mangle_expression_ref(expression.expression_refs.at(1));
    return expression_operator_code(expression.op) + left + right;
  }
  case ABI_EXPRESSION_CONDITIONAL: {
    std::string result = "qu";
    for(std::size_t i = 0; i < 3; ++i) {
      result += mangle_expression_ref(expression.expression_refs.at(i));
    }
    return result;
  }
  case ABI_EXPRESSION_PACK_EXPANSION:
    return "sp" + mangle_expression_ref(expression.expression_refs.at(0));
  case ABI_EXPRESSION_CALL: {
    std::string result = "cl";
    for(std::size_t i = 0; i < expression.expression_refs.size(); ++i) {
      result += mangle_expression_ref(expression.expression_refs[i]);
    }
    return result + "E";
  }
  case ABI_EXPRESSION_CONVERSION:
  case ABI_EXPRESSION_CAST: {
    const std::string operation = expression.op.empty() ? std::string("cv") :
      expression_operator_code(expression.op);
    const std::string type = mangle_type_impl(expression.type, false);
    return operation + type + mangle_expression_ref(expression.expression_refs.at(0));
  }
  case ABI_EXPRESSION_TEMPLATE_ID: {
    bool nested = needs_nested_name(name_components(expression.text));
    const AbiKeyId key = name_key(expression.text, std::vector<std::string>());
    std::string result;
    if(!substitutions_.lookup(key, &result)) {
      result = mangle_name_body(expression.text, std::vector<std::string>(), key,
                                &nested);
    }
    result += mangle_template_args(expression.argument_refs);
    return nested ? "N" + result + "E" : result;
  }
  case ABI_EXPRESSION_TYPE_TRAIT: {
    std::string result = "u" + source_name(expression.text);
    for(std::size_t i = 0; i < expression.type_arguments.size(); ++i) {
      result += mangle_type_impl(expression.type_arguments[i], false);
    }
    return result + "E";
  }
  case ABI_EXPRESSION_SIZEOF_TYPE:
    return "st" + mangle_type_impl(expression.type, false);
  case ABI_EXPRESSION_MEMBER: {
    std::string result = "sr" + mangle_type_impl(expression.type, false);
    if(expression.close_member_owner) result += "E";
    result += source_name(expression.text);
    if(!expression.argument_refs.empty()) {
      result += mangle_template_args(expression.argument_refs);
    }
    return result;
  }
  case ABI_EXPRESSION_OBJECT_MEMBER: {
    const std::string object = mangle_expression_ref(expression.expression_refs.at(0));
    std::string result = expression_operator_code(expression.op) + object +
      source_name(expression.text);
    if(!expression.argument_refs.empty()) {
      result += mangle_template_args(expression.argument_refs);
    }
    return result;
  }
  case ABI_EXPRESSION_EXTERNAL_ENTITY:
  case ABI_EXPRESSION_ENTITY:
    return "L" + mangle_entity_encoding(entity_definition(expression.entity_ref)) +
      "E";
  }
  throw std::logic_error("unknown ABI expression kind");
}

// ---------------------------------------------------------------------------
// Shared name helpers used by the type path

AbiKeyId Mangler::name_key(const std::string & qualified_name,
                           const std::vector<std::string> & tags)
{
  return keys_.intern("N:" + strip_scope(qualified_name) + canonical_tags(tags));
}

std::string Mangler::mangle_tag_list(const std::vector<std::string> & tags) const
{
  return canonical_tags(tags);
}

}  // namespace abi_mangle
