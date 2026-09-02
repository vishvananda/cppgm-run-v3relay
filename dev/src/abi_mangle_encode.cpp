// Itanium <name>, function encodings, special names, and the target driver.

#include "abi_mangle_encoder.h"

#include <stdexcept>

namespace abi_mangle {
namespace {

std::string number_word(unsigned long long value)
{
  return std::to_string(value);
}

std::string signed_number(long long value)
{
  if(value >= 0) {
    return number_word(static_cast<unsigned long long>(value));
  }
  return "n" + number_word(0ULL - static_cast<unsigned long long>(value));
}

std::string source_name(const std::string & name)
{
  return number_word(name.size()) + name;
}

bool is_dash(const std::string & word)
{
  return word.empty() || word == "-";
}

std::string strip_scope(const std::string & name)
{
  return name.compare(0, 2, "::") == 0 ? name.substr(2) : name;
}

std::string joined_components(const std::vector<std::string> & components,
                              std::size_t count)
{
  std::string result;
  for(std::size_t i = 0; i < count; ++i) {
    if(i != 0) result += "::";
    result += components[i];
  }
  return result;
}

bool is_standard(const std::vector<std::string> & components)
{
  return !components.empty() && components[0] == "std";
}

void append_qualifier_codes(const std::vector<AbiFunctionQualifier> & qualifiers,
                            std::string * out)
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
  if(has_volatile) *out += "V";
  if(has_const) *out += "K";
  if(has_lvalue_ref) *out += "R";
  if(has_rvalue_ref) *out += "O";
}

// Local class discriminators count occurrences; the first is unnumbered.
std::string local_discriminator_suffix(const std::string & discriminator)
{
  if(is_dash(discriminator)) {
    return std::string();
  }
  const unsigned long long value = std::stoull(discriminator);
  return value == 0 ? std::string() : "_" + number_word(value - 1);
}

}  // namespace

std::vector<std::string> name_components(const std::string & name)
{
  const std::string normalized = strip_scope(name);
  std::vector<std::string> components;
  std::size_t start = 0;
  while(true) {
    const std::size_t separator = normalized.find("::", start);
    const std::size_t end = separator == std::string::npos ? normalized.size() :
      separator;
    if(end == start) {
      throw std::logic_error("ABI qualified name '" + name +
        "' has an empty component");
    }
    components.push_back(normalized.substr(start, end - start));
    if(separator == std::string::npos) {
      return components;
    }
    start = separator + 2;
  }
}

bool needs_nested_name(const std::vector<std::string> & components)
{
  if(components.size() == 1) return false;
  return !(components.size() == 2 && is_standard(components));
}

// ---------------------------------------------------------------------------
// Definition table

void AbiDefinitionTable::add(const std::string & id, AbiDefinitionKind kind,
                             const void * fact)
{
  Entry entry;
  entry.kind = kind;
  entry.fact = fact;
  if(!entries_.insert(std::make_pair(id, entry)).second) {
    throw std::logic_error("duplicate ABI definition '" + id + "'");
  }
}

const void * AbiDefinitionTable::find(const std::string & id,
                                      AbiDefinitionKind kind) const
{
  const std::map<std::string, Entry>::const_iterator it = entries_.find(id);
  if(it == entries_.end() || it->second.kind != kind) {
    return 0;
  }
  return it->second.fact;
}

void AbiDefinitionTable::add_type(const std::string & id, const AbiType & type)
{
  add(id, ABI_DEFINITION_TYPE, &type);
}

void AbiDefinitionTable::add_argument(const std::string & id,
                                      const AbiTemplateArgument & argument)
{
  add(id, ABI_DEFINITION_TEMPLATE_ARGUMENT, &argument);
}

void AbiDefinitionTable::add_expression(const std::string & id,
                                        const AbiDependentExpression & expression)
{
  add(id, ABI_DEFINITION_EXPRESSION, &expression);
}

void AbiDefinitionTable::add_context(const std::string & id,
                                     const AbiLocalContext & context)
{
  add(id, ABI_DEFINITION_CONTEXT, &context);
}

void AbiDefinitionTable::add_entity(const std::string & id,
                                    const AbiEntityFact & entity)
{
  add(id, ABI_DEFINITION_ENTITY, &entity);
}

void AbiDefinitionTable::add_case(const AbiFactCase & fact_case)
{
  for(std::size_t i = 0; i < fact_case.types.size(); ++i) {
    add_type(fact_case.types[i].id, fact_case.types[i].type);
  }
  for(std::size_t i = 0; i < fact_case.arguments.size(); ++i) {
    add_argument(fact_case.arguments[i].id, fact_case.arguments[i].argument);
  }
  for(std::size_t i = 0; i < fact_case.expressions.size(); ++i) {
    add_expression(fact_case.expressions[i].id,
                   fact_case.expressions[i].expression);
  }
  for(std::size_t i = 0; i < fact_case.contexts.size(); ++i) {
    add_context(fact_case.contexts[i].id, fact_case.contexts[i].context);
  }
  for(std::size_t i = 0; i < fact_case.entities.size(); ++i) {
    add_entity(fact_case.entities[i].id, fact_case.entities[i].entity);
  }
}

const AbiType * AbiDefinitionTable::find_type(const std::string & id) const
{
  return static_cast<const AbiType *>(find(id, ABI_DEFINITION_TYPE));
}

const AbiTemplateArgument * AbiDefinitionTable::find_argument(
  const std::string & id) const
{
  return static_cast<const AbiTemplateArgument *>(
    find(id, ABI_DEFINITION_TEMPLATE_ARGUMENT));
}

const AbiDependentExpression * AbiDefinitionTable::find_expression(
  const std::string & id) const
{
  return static_cast<const AbiDependentExpression *>(
    find(id, ABI_DEFINITION_EXPRESSION));
}

const AbiLocalContext * AbiDefinitionTable::find_context(
  const std::string & id) const
{
  return static_cast<const AbiLocalContext *>(find(id, ABI_DEFINITION_CONTEXT));
}

const AbiEntityFact * AbiDefinitionTable::find_entity(const std::string & id) const
{
  return static_cast<const AbiEntityFact *>(find(id, ABI_DEFINITION_ENTITY));
}

// ---------------------------------------------------------------------------
// <name> and <prefix>

AbiKeyId Mangler::prefix_key(const std::vector<std::string> & components,
                             std::size_t count)
{
  return name_key(joined_components(components, count), std::vector<std::string>());
}

// Emits the first `count` components of a qualified name as <prefix>es: the
// longest already-registered prefix is substituted, every prefix emitted
// after it is registered, and a leading std is spelled St and never
// registered.
void Mangler::append_prefixes(const std::vector<std::string> & components,
                              std::size_t count, std::string * out)
{
  const bool standard = is_standard(components);
  const std::size_t first = standard ? 1 : 0;
  std::size_t start = first;
  bool substituted = false;
  for(std::size_t length = count; length > first; --length) {
    std::string spelling;
    if(substitutions_.lookup(prefix_key(components, length), &spelling)) {
      *out += spelling;
      start = length;
      substituted = true;
      break;
    }
  }
  if(standard && !substituted && count > 0) {
    *out += "St";
  }
  for(std::size_t length = start + 1; length <= count; ++length) {
    *out += source_name(components[length - 1]);
    substitutions_.add(prefix_key(components, length));
  }
}

// Body of a qualified name without the N...E wrapper.  `register_key` is the
// component the complete name denotes (a class, template, or closure) and is
// registered after the name is emitted; 0 for functions and variables.
std::string Mangler::mangle_name_body(const std::string & qualified_name,
                                      const std::vector<std::string> & tags,
                                      AbiKeyId register_key, bool * nested)
{
  const std::vector<std::string> components = name_components(qualified_name);
  std::string result;
  append_prefixes(components, components.size() - 1, &result);
  result += source_name(components.back());
  result += mangle_tag_list(tags);
  substitutions_.add(register_key);
  *nested = needs_nested_name(components);
  return result;
}

std::string Mangler::mangle_qualified_name(const std::string & qualified_name)
{
  bool nested = false;
  const std::string body = mangle_name_body(qualified_name,
    std::vector<std::string>(), 0, &nested);
  return nested ? "N" + body + "E" : body;
}

std::string Mangler::mangle_internal_name(const std::string & qualified_name)
{
  const std::vector<std::string> components = name_components(qualified_name);
  if(components.size() == 1) {
    return "L" + source_name(components[0]);
  }
  std::string result = "N";
  append_prefixes(components, components.size() - 1, &result);
  return result + "L" + source_name(components.back()) + "E";
}

// ---------------------------------------------------------------------------
// Functions

void Mangler::lower_records(const std::vector<AbiFunctionRecord> & records,
                            FunctionFacts * facts)
{
  for(std::size_t i = 0; i < records.size(); ++i) {
    const AbiFunctionRecord & record = records[i];
    switch(record.kind) {
    case ABI_FUNCTION_RECORD_NAME_SOURCE: {
      if(is_dash(record.source_name)) break;   // ctor/dtor placeholder
      NamePiece piece;
      piece.kind = NamePiece::SOURCE;
      piece.spelling = &record.source_name;
      piece.key = is_dash(record.substitution) ? 0 :
        name_key(record.substitution, std::vector<std::string>());
      facts->pieces.push_back(piece);
      break;
    }
    case ABI_FUNCTION_RECORD_NAME_STD:
      facts->standard = true;
      facts->standard_substitution = record.standard_substitution.empty() ?
        std::string("St") : record.standard_substitution;
      break;
    case ABI_FUNCTION_RECORD_NAME_TEMPLATE: {
      NamePiece piece;
      piece.kind = NamePiece::TEMPLATE;
      piece.spelling = &record.name;
      piece.key = is_dash(record.substitution) ? 0 :
        name_key(record.substitution, std::vector<std::string>());
      piece.complete_key = is_dash(record.complete_substitution) ? 0 :
        name_key(record.complete_substitution, std::vector<std::string>());
      piece.standard_substitution = is_dash(record.standard_substitution) ? 0 :
        &record.standard_substitution;
      piece.standard_substitution_includes_arguments =
        record.standard_substitution_includes_arguments;
      piece.argument_refs = &record.argument_refs;
      facts->pieces.push_back(piece);
      break;
    }
    case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT:
      facts->template_arguments.push_back(record.argument_refs.at(0));
      facts->template_encoding = true;
      break;
    case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX:
      if(!is_dash(record.substitution)) {
        facts->template_prefix_keys.push_back(
          name_key(record.substitution, std::vector<std::string>()));
      }
      break;
    case ABI_FUNCTION_RECORD_LOCAL_CONTEXT:
    case ABI_FUNCTION_RECORD_LAMBDA_CONTEXT:
    case ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT:
      if(facts->context) {
        throw std::logic_error("ABI function has more than one local context");
      }
      facts->context = &record;
      break;
    case ABI_FUNCTION_RECORD_TERMINAL:
      if(facts->terminal) {
        throw std::logic_error("multiple ABI function terminals");
      }
      facts->terminal = &record.terminal;
      facts->conversion_type = &record.type;
      break;
    case ABI_FUNCTION_RECORD_VARIADIC:
      facts->variadic = true;
      break;
    case ABI_FUNCTION_RECORD_ABI_TAG:
      facts->tags.push_back(record.name);
      break;
    case ABI_FUNCTION_RECORD_QUALIFIER:
      facts->qualifiers.insert(facts->qualifiers.end(),
                               record.qualifiers.begin(), record.qualifiers.end());
      break;
    case ABI_FUNCTION_RECORD_PARAMETER:
      facts->parameters.push_back(&record.type);
      break;
    case ABI_FUNCTION_RECORD_RESULT:
      facts->results.push_back(&record.type);
      break;
    }
  }
}

// A compact `function [path] Q operands...` target: the qualified name
// supplies the name pieces and the operands supply template arguments and,
// unless param records follow, the parameter types.
void Mangler::lower_path_target(const AbiFunctionTarget & target,
                                const std::vector<AbiFunctionRecord> & records,
                                FunctionFacts * facts,
                                std::vector<std::string> * components)
{
  lower_records(records, facts);
  std::vector<const AbiType *> path_parameters;
  for(std::size_t i = 0; i < target.path_operands.size(); ++i) {
    const AbiFunctionPathOperand & operand = target.path_operands[i];
    if(operand.kind == ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT) {
      facts->template_arguments.push_back(operand.argument_ref);
    } else if(operand.kind == ABI_FUNCTION_PATH_TYPE) {
      path_parameters.push_back(&operand.type);
    } else {
      facts->variadic = true;
    }
  }
  if(facts->parameters.empty()) {
    facts->parameters = path_parameters;
  }
  facts->template_encoding = !facts->template_arguments.empty();

  *components = name_components(target.qualified_name);
  std::size_t first = 0;
  if(is_standard(*components)) {
    facts->standard = true;
    facts->standard_substitution = "St";
    first = 1;
  }
  // With a terminal the last component is its placeholder.
  const std::size_t count = facts->terminal ? components->size() - 1 :
    components->size();
  for(std::size_t i = first; i < count; ++i) {
    NamePiece piece;
    piece.kind = NamePiece::SOURCE;
    piece.spelling = &(*components)[i];
    const bool last = i + 1 == components->size();
    if(!last || facts->template_encoding) {
      piece.key = prefix_key(*components, i + 1);
    }
    facts->pieces.push_back(piece);
  }
}

std::string Mangler::mangle_function(const AbiFunctionTarget & target,
                                     const std::vector<AbiFunctionRecord> & records)
{
  DepthScope scope(this);
  FunctionFacts facts;
  std::vector<std::string> components;
  AbiFunctionRecord owned_context;
  switch(target.kind) {
  case ABI_FUNCTION_TARGET_PATH:
    lower_path_target(target, records, &facts, &components);
    break;
  case ABI_FUNCTION_TARGET_ENCODING:
    lower_records(records, &facts);
    break;
  case ABI_FUNCTION_TARGET_LOCAL:
  case ABI_FUNCTION_TARGET_LAMBDA:
  case ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA:
    owned_context.kind = target.kind == ABI_FUNCTION_TARGET_LOCAL ?
      ABI_FUNCTION_RECORD_LOCAL_CONTEXT :
      target.kind == ABI_FUNCTION_TARGET_LAMBDA ?
      ABI_FUNCTION_RECORD_LAMBDA_CONTEXT :
      ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT;
    owned_context.context_ref = target.context_ref;
    owned_context.source_name = target.source_name;
    owned_context.discriminator = target.discriminator;
    owned_context.types = target.signature_parameter_types;
    owned_context.namespace_qualifiers = target.namespace_qualifiers;
    lower_records(records, &facts);
    if(facts.context || facts.terminal) {
      throw std::logic_error("compact local function targets carry their own "
        "context and terminal");
    }
    facts.context = &owned_context;
    facts.terminal = &target.terminal;
    break;
  }
  return mangle_function_facts(facts);
}

std::string Mangler::mangle_function_facts(const FunctionFacts & facts)
{
  std::string result = facts.context ? mangle_context_function_name(facts) :
    mangle_function_name(facts);
  // A function whose own name is a template-id, or that carries function
  // template arguments, encodes its return type; constructors, destructors,
  // and conversion functions never do.
  const bool template_id = !facts.terminal && !facts.pieces.empty() &&
    facts.pieces.back().kind == NamePiece::TEMPLATE;
  const bool special = facts.terminal &&
    (facts.terminal->kind == ABI_TERMINAL_SPECIAL ||
     facts.terminal->kind == ABI_TERMINAL_CONVERSION);
  if((facts.template_encoding || template_id) && !special) {
    for(std::size_t i = 0; i < facts.results.size(); ++i) {
      result += mangle_type(*facts.results[i]);
    }
  }
  if(facts.parameters.empty()) {
    result += "v";
  }
  for(std::size_t i = 0; i < facts.parameters.size(); ++i) {
    result += mangle_type(*facts.parameters[i]);
  }
  if(facts.variadic) result += "z";
  return result;
}

// A template piece registers its template name before the arguments; only a
// prefix piece (a class template specialization) also registers the complete
// specialization, since function template specializations are not
// substitution candidates.
void Mangler::append_name_piece(const NamePiece & piece, bool prefix,
                                std::string * out)
{
  if(piece.kind == NamePiece::SOURCE) {
    *out += source_name(*piece.spelling);
    substitutions_.add(piece.key);
    return;
  }
  if(piece.standard_substitution) {
    *out += *piece.standard_substitution;
    substitutions_.add(piece.key);
    if(!piece.standard_substitution_includes_arguments) {
      *out += mangle_template_args(*piece.argument_refs);
    }
  } else {
    std::string spelling;
    if(substitutions_.lookup(piece.key, &spelling)) {
      *out += spelling;
    } else {
      *out += source_name(*piece.spelling);
      substitutions_.add(piece.key);
    }
    *out += mangle_template_args(*piece.argument_refs);
  }
  if(prefix) {
    substitutions_.add(piece.complete_key);
  }
}

std::string Mangler::mangle_terminal(const FunctionFacts & facts, bool member)
{
  const AbiTerminal & terminal = *facts.terminal;
  switch(terminal.kind) {
  case ABI_TERMINAL_SOURCE:
    if(is_dash(terminal.name)) {
      throw std::logic_error("empty function terminal source");
    }
    return source_name(terminal.name);
  case ABI_TERMINAL_SPECIAL:
    return special_function_info(terminal.special_function).code;
  case ABI_TERMINAL_OPERATOR: {
    const AbiOperatorInfo & info = operator_info(terminal.operator_kind);
    if(info.unary_code) {
      const std::size_t operands = facts.parameters.size() + (member ? 1 : 0);
      return operands == 1 ? info.unary_code : info.code;
    }
    return info.code;
  }
  case ABI_TERMINAL_LITERAL_OPERATOR:
    return "li" + source_name(terminal.name);
  case ABI_TERMINAL_CONVERSION:
    return "cv" + mangle_type(*facts.conversion_type);
  }
  throw std::logic_error("invalid ABI function terminal");
}

// <nested-name> or <unscoped-name> of a function from its name pieces.
std::string Mangler::mangle_function_name(const FunctionFacts & facts)
{
  std::vector<NamePiece> pieces = facts.pieces;
  const AbiTerminal * terminal = facts.terminal;
  // A source-level "operator" component is a placeholder when a terminal
  // supplies the ABI unqualified name.
  if(terminal && !pieces.empty() && pieces.back().kind == NamePiece::SOURCE &&
     *pieces.back().spelling == "operator") {
    pieces.pop_back();
  }
  if(pieces.empty() && !terminal) {
    throw std::logic_error("ABI function has no name");
  }
  const std::size_t prefix_count = terminal ? pieces.size() : pieces.size() - 1;
  const std::size_t component_count = (facts.standard ? 1 : 0) + pieces.size() +
    (terminal ? 1 : 0);
  const bool nested = component_count > 1 &&
    !(facts.standard && !terminal && pieces.size() == 1 &&
      facts.qualifiers.empty());

  std::string result;
  if(nested) result += "N";
  append_qualifier_codes(facts.qualifiers, &result);
  std::size_t start = 0;
  bool substituted = false;
  for(std::size_t length = prefix_count; length > 0; --length) {
    const NamePiece & piece = pieces[length - 1];
    const AbiKeyId key = piece.kind == NamePiece::TEMPLATE ? piece.complete_key :
      piece.key;
    std::string spelling;
    if(substitutions_.lookup(key, &spelling)) {
      result += spelling;
      start = length;
      substituted = true;
      break;
    }
  }
  if(facts.standard && !substituted) {
    result += facts.standard_substitution;
  }
  for(std::size_t i = start; i < prefix_count; ++i) {
    append_name_piece(pieces[i], true, &result);
  }
  if(terminal) {
    result += mangle_terminal(facts, prefix_count > 0);
  } else {
    append_name_piece(pieces.back(), false, &result);
  }
  result += mangle_tag_list(facts.tags);
  for(std::size_t i = 0; i < facts.template_prefix_keys.size(); ++i) {
    substitutions_.add(facts.template_prefix_keys[i]);
  }
  if(!facts.template_arguments.empty()) {
    result += mangle_template_args(facts.template_arguments);
  }
  if(nested) result += "E";
  return result;
}

// <local-name> forms: a member of a local class, a closure call operator, or
// a namespace-scope closure call operator.  The local class or closure type
// is a <prefix> candidate keyed exactly as the corresponding type fact.
std::string Mangler::mangle_context_function_name(const FunctionFacts & facts)
{
  const AbiFunctionRecord & context = *facts.context;
  std::string result;
  AbiKeyId owner_key = 0;
  if(context.kind == ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT) {
    result = "N";
    append_qualifier_codes(facts.qualifiers, &result);
    append_prefixes(context.namespace_qualifiers,
                    context.namespace_qualifiers.size(), &result);
    result += source_name(context.source_name);
    owner_key = namespace_lambda_key(context.namespace_qualifiers,
                                     context.source_name);
  } else {
    result = mangle_context(context.context_ref) + "N";
    append_qualifier_codes(facts.qualifiers, &result);
    if(context.kind == ABI_FUNCTION_RECORD_LOCAL_CONTEXT) {
      result += source_name(context.source_name) +
        local_discriminator_suffix(context.discriminator);
      owner_key = local_type_key(context.context_ref, context.source_name,
                                 context.discriminator);
    } else {
      result += "Ul";
      if(context.types.empty()) result += "v";
      for(std::size_t i = 0; i < context.types.size(); ++i) {
        result += mangle_type(context.types[i]);
      }
      result += "E";
      if(!is_dash(context.discriminator)) result += context.discriminator;
      result += "_";
      owner_key = lambda_closure_key(context.context_ref, context.discriminator,
                                     context.types);
    }
  }
  substitutions_.add(owner_key);
  if(facts.terminal) {
    result += mangle_terminal(facts, true);
  } else {
    result += "cl";
  }
  result += mangle_tag_list(facts.tags);
  for(std::size_t i = 0; i < facts.template_prefix_keys.size(); ++i) {
    substitutions_.add(facts.template_prefix_keys[i]);
  }
  if(!facts.template_arguments.empty()) {
    result += mangle_template_args(facts.template_arguments);
  }
  return result + "E";
}

// Z <encoding> E of a local-name context, sharing this name's substitution
// table; a raw context is an already-normalized fragment.
std::string Mangler::mangle_context(const std::string & ref)
{
  DepthScope scope(this);
  const AbiLocalContext & context = context_definition(ref);
  if(context.kind == ABI_CONTEXT_RAW) {
    return context.fragment;
  }
  return "Z" + mangle_function(context.function, std::vector<AbiFunctionRecord>()) +
    "E";
}

// ---------------------------------------------------------------------------
// Entities

// Entity operands are encoded with a fresh substitution table: the fixtures
// pin that nothing inside L_Z...E is visible to the enclosing name.
std::string Mangler::mangle_entity_encoding(const AbiEntityFact & entity)
{
  DepthScope scope(this);
  Mangler nested(definitions_, depth_);
  return nested.mangle_entity_impl(entity);
}

std::string Mangler::mangle_entity_impl(const AbiEntityFact & entity)
{
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return entity.qualified_name;
  }
  if(entity.kind == ABI_ENTITY_FACT_FUNCTION) {
    return "_Z" + mangle_function(entity.function,
                                  std::vector<AbiFunctionRecord>());
  }
  return "_Z" + (entity.internal_linkage ?
    mangle_internal_name(entity.qualified_name) :
    mangle_qualified_name(entity.qualified_name));
}

// ---------------------------------------------------------------------------
// Targets

std::string Mangler::mangle_special_target(const AbiTargetRecord & target)
{
  switch(target.kind) {
  case ABI_TARGET_FACT_TYPE:
    return mangle_type(target.type);
  case ABI_TARGET_FACT_TYPEINFO:
    return "_ZTI" + mangle_type(target.type);
  case ABI_TARGET_FACT_VTABLE:
    return "_ZTV" + mangle_type(target.type);
  case ABI_TARGET_FACT_VTT:
    return "_ZTT" + mangle_type(target.type);
  case ABI_TARGET_FACT_CONSTRUCTION_VTABLE:
    return "_ZTC" + mangle_type(target.type) + number_word(target.base_offset) +
      "_" + mangle_type(target.base_type);
  case ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER:
    return "_ZTW" + mangle_qualified_name(target.qualified_name);
  case ABI_TARGET_FACT_VARIABLE:
    return "_Z" + (target.internal_linkage ?
      mangle_internal_name(target.qualified_name) :
      mangle_qualified_name(target.qualified_name));
  default:
    throw std::logic_error("unsupported ABI target");
  }
}

// <special-name> thunks: Th <call-offset> and Tc <call-offset> <call-offset>,
// where a call offset is h <fixed> _ or v <fixed> _ <vcall offset> _.
std::string Mangler::mangle_thunk(const AbiTargetRecord & target,
                                  const std::string & encoding) const
{
  if(target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
    return "_ZTv" + signed_number(target.this_adjust) + "_" +
      signed_number(target.vcall_offset) + "_" + encoding;
  }
  std::string result = target.has_result_adjust ? "_ZTc" : "_ZT";
  result += "h" + signed_number(target.this_adjust) + "_";
  if(target.has_result_adjust) {
    if(target.result_adjust_virtual) {
      result += "v" + signed_number(target.result_adjust) + "_" +
        signed_number(target.result_vcall_offset) + "_";
    } else {
      result += "h" + signed_number(target.result_adjust) + "_";
    }
  }
  return result + encoding;
}

std::string Mangler::mangle_target(const AbiTargetRecord & target,
                                   const std::vector<AbiFunctionRecord> & records)
{
  if(target.kind == ABI_TARGET_FACT_FUNCTION) {
    if(target.c_linkage) return target.function.qualified_name;
    return "_Z" + mangle_function(target.function, records);
  }
  if(target.kind == ABI_TARGET_FACT_THUNK ||
     target.kind == ABI_TARGET_FACT_VIRTUAL_BASE_THUNK) {
    return mangle_thunk(target, mangle_function(target.function, records));
  }
  return mangle_special_target(target);
}

std::string mangle_target(const AbiTargetRecord & target,
                          const std::vector<AbiFunctionRecord> & function_records,
                          const AbiDefinitionTable & definitions)
{
  return Mangler(definitions, 0).mangle_target(target, function_records);
}

std::string mangle_fact_case(const AbiFactCase & fact_case)
{
  if(!fact_case.has_target) {
    throw std::logic_error("ABI case must contain exactly one target");
  }
  AbiDefinitionTable definitions;
  definitions.add_case(fact_case);
  return mangle_target(fact_case.target, fact_case.function_records, definitions);
}

}  // namespace abi_mangle
