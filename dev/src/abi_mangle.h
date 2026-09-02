#pragma once

// ABI fact model for the PA14 standalone abimangle tool.

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace abi_mangle {

enum AbiFactRecordKind
{
  ABI_FACT_RECORD_DEFINITION,
  ABI_FACT_RECORD_TARGET,
  ABI_FACT_RECORD_FUNCTION
};

enum AbiDefinitionKind
{
  ABI_DEFINITION_TYPE,
  ABI_DEFINITION_TEMPLATE_ARGUMENT,
  ABI_DEFINITION_EXPRESSION,
  ABI_DEFINITION_CONTEXT,
  ABI_DEFINITION_ENTITY
};

enum AbiTypeKind
{
  ABI_TYPE_NAME_OR_REFERENCE,
  ABI_TYPE_NAMED,
  ABI_TYPE_BUILTIN,
  ABI_TYPE_TEMPLATE_PARAMETER,
  ABI_TYPE_POINTER,
  ABI_TYPE_LVALUE_REFERENCE,
  ABI_TYPE_RVALUE_REFERENCE,
  ABI_TYPE_CV,
  ABI_TYPE_PACK_EXPANSION,
  ABI_TYPE_VENDOR_QUALIFIED,
  ABI_TYPE_ARRAY,
  ABI_TYPE_BUILTIN_TRANSFORM,
  ABI_TYPE_FUNCTION,
  ABI_TYPE_MEMBER_POINTER,
  ABI_TYPE_TEMPLATE_SPECIALIZATION,
  ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION,
  ABI_TYPE_STD_TEMPLATE_SPECIALIZATION,
  ABI_TYPE_MEMBER,
  ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION,
  ABI_TYPE_DECLTYPE_EXPRESSION,
  ABI_TYPE_LAMBDA_CLOSURE,
  ABI_TYPE_LOCAL_TYPE,
  ABI_TYPE_NAMESPACE_LAMBDA
};

enum AbiArrayBoundKind
{
  ABI_ARRAY_BOUND_VALUE,
  ABI_ARRAY_BOUND_RAW,
  ABI_ARRAY_BOUND_EXPRESSION
};

enum AbiTemplateArgumentKind
{
  ABI_TEMPLATE_ARGUMENT_TYPE,
  ABI_TEMPLATE_ARGUMENT_VALUE,
  ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE,
  ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE,
  ABI_TEMPLATE_ARGUMENT_EXPRESSION,
  ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY,
  ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY,
  ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE,
  ABI_TEMPLATE_ARGUMENT_EXTERNAL_ENTITY,
  ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY,
  ABI_TEMPLATE_ARGUMENT_ENTITY,
  ABI_TEMPLATE_ARGUMENT_PACK
};

enum AbiExpressionKind
{
  ABI_EXPRESSION_TEMPLATE_PARAMETER,
  ABI_EXPRESSION_FUNCTION_PARAMETER,
  ABI_EXPRESSION_LITERAL,
  ABI_EXPRESSION_INTEGRAL_VALUE,
  ABI_EXPRESSION_UNARY,
  ABI_EXPRESSION_BINARY,
  ABI_EXPRESSION_CONDITIONAL,
  ABI_EXPRESSION_PACK_EXPANSION,
  ABI_EXPRESSION_CALL,
  ABI_EXPRESSION_CONVERSION,
  ABI_EXPRESSION_CAST,
  ABI_EXPRESSION_TEMPLATE_ID,
  ABI_EXPRESSION_TYPE_TRAIT,
  ABI_EXPRESSION_SIZEOF_TYPE,
  ABI_EXPRESSION_MEMBER,
  ABI_EXPRESSION_OBJECT_MEMBER,
  ABI_EXPRESSION_EXTERNAL_ENTITY,
  ABI_EXPRESSION_ENTITY
};

enum AbiContextFactKind
{
  ABI_CONTEXT_RAW,
  ABI_CONTEXT_FUNCTION
};

enum AbiEntityFactKind
{
  ABI_ENTITY_FACT_FUNCTION,
  ABI_ENTITY_FACT_VARIABLE,
  ABI_ENTITY_FACT_SYMBOL
};

enum AbiTargetFactKind
{
  ABI_TARGET_FACT_TYPE,
  ABI_TARGET_FACT_FUNCTION,
  ABI_TARGET_FACT_VARIABLE,
  ABI_TARGET_FACT_TYPEINFO,
  ABI_TARGET_FACT_VTABLE,
  ABI_TARGET_FACT_VTT,
  ABI_TARGET_FACT_CONSTRUCTION_VTABLE,
  ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER,
  ABI_TARGET_FACT_THUNK,
  ABI_TARGET_FACT_VIRTUAL_BASE_THUNK
};

enum AbiFunctionTargetKind
{
  ABI_FUNCTION_TARGET_PATH,
  ABI_FUNCTION_TARGET_ENCODING,
  ABI_FUNCTION_TARGET_LAMBDA,
  ABI_FUNCTION_TARGET_LOCAL,
  ABI_FUNCTION_TARGET_NAMESPACE_LAMBDA
};

enum AbiFunctionPathOperandKind
{
  ABI_FUNCTION_PATH_TYPE,
  ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT,
  ABI_FUNCTION_PATH_RESULT_TYPE,
  ABI_FUNCTION_PATH_VARIADIC
};

enum AbiFunctionRecordKind
{
  ABI_FUNCTION_RECORD_NAME_SOURCE,
  ABI_FUNCTION_RECORD_NAME_STD,
  ABI_FUNCTION_RECORD_NAME_TEMPLATE,
  ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT,
  ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX,
  ABI_FUNCTION_RECORD_LOCAL_CONTEXT,
  ABI_FUNCTION_RECORD_LAMBDA_CONTEXT,
  ABI_FUNCTION_RECORD_NAMESPACE_LAMBDA_CONTEXT,
  ABI_FUNCTION_RECORD_TERMINAL_SOURCE,
  ABI_FUNCTION_RECORD_TERMINAL,
  ABI_FUNCTION_RECORD_VARIADIC,
  ABI_FUNCTION_RECORD_ABI_TAG,
  ABI_FUNCTION_RECORD_QUALIFIER,
  ABI_FUNCTION_RECORD_OPERATOR_TERMINAL,
  ABI_FUNCTION_RECORD_CONVERSION_TERMINAL,
  ABI_FUNCTION_RECORD_PARAMETER,
  ABI_FUNCTION_RECORD_RESULT
};

enum AbiFunctionQualifier
{
  ABI_FUNCTION_QUALIFIER_CONST,
  ABI_FUNCTION_QUALIFIER_VOLATILE,
  ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE,
  ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE
};

struct AbiArrayBound
{
  AbiArrayBoundKind kind = ABI_ARRAY_BOUND_VALUE;
  std::string value;
};

struct AbiType
{
  AbiTypeKind kind = ABI_TYPE_NAME_OR_REFERENCE;
  std::string name;
  std::string substitution;
  std::string standard_substitution;
  std::string expression_ref;
  std::string context_ref;
  std::string discriminator;
  AbiArrayBound array_bound;
  std::size_t index = 0;
  bool is_const = false;
  bool is_volatile = false;
  bool variadic = false;
  bool lvalue_ref = false;
  bool rvalue_ref = false;
  bool substitutable = false;
  bool standard_substitution_includes_arguments = false;
  bool tagged = false;
  std::vector<AbiType> types;
  std::vector<std::string> argument_refs;
  std::vector<std::string> namespace_qualifiers;
  std::vector<std::string> abi_tags;
};

struct AbiTemplateArgument
{
  AbiTemplateArgumentKind kind = ABI_TEMPLATE_ARGUMENT_TYPE;
  AbiType type;
  AbiType value_type;
  AbiType owner_type;
  std::string name;
  std::string substitution;
  std::string entity_ref;
  std::string expression_ref;
  std::string symbol;
  long long value = 0;
  std::size_t index = 0;
  bool has_value_type = false;
  bool address_of = false;
  bool member_is_function = false;
  bool member_function_const = false;
  bool member_function_volatile = false;
  bool member_function_lvalue_ref = false;
  bool member_function_rvalue_ref = false;
  bool member_function_variadic = false;
  std::vector<AbiType> parameter_types;
  std::vector<std::string> argument_refs;
};

struct AbiDependentExpression
{
  AbiExpressionKind kind = ABI_EXPRESSION_LITERAL;
  AbiType type;
  AbiType value_type;
  std::string text;
  std::string op;
  std::string entity_ref;
  long long value = 0;
  std::size_t index = 0;
  bool close_member_owner = false;
  bool address_of = false;
  std::vector<std::string> expression_refs;
  std::vector<std::string> argument_refs;
  std::vector<AbiType> type_arguments;
};

struct AbiFunctionPathOperand
{
  AbiFunctionPathOperandKind kind = ABI_FUNCTION_PATH_TYPE;
  AbiType type;
  std::string argument_ref;
};

struct AbiFunctionTarget
{
  AbiFunctionTargetKind kind = ABI_FUNCTION_TARGET_PATH;
  std::string qualified_name;
  std::string context_ref;
  std::string source_name;
  std::string discriminator;
  std::string terminal;
  std::vector<AbiFunctionPathOperand> path_operands;
  std::vector<AbiType> signature_parameter_types;
  std::vector<std::string> namespace_qualifiers;
};

struct AbiLocalContext
{
  AbiContextFactKind kind = ABI_CONTEXT_RAW;
  std::string fragment;
  AbiFunctionTarget function;
};

struct AbiEntityFact
{
  AbiEntityFactKind kind = ABI_ENTITY_FACT_VARIABLE;
  std::string qualified_name;
  AbiFunctionTarget function;
  bool internal_linkage = false;
};

struct AbiDefinitionRecord
{
  AbiDefinitionKind kind = ABI_DEFINITION_TYPE;
  std::string id;
  AbiType type;
  AbiTemplateArgument template_argument;
  AbiDependentExpression expression;
  AbiLocalContext context;
  AbiEntityFact entity;
};

struct AbiTargetRecord
{
  AbiTargetFactKind kind = ABI_TARGET_FACT_TYPE;
  bool c_linkage = false;
  AbiType type;
  AbiType base_type;
  AbiFunctionTarget function;
  std::string qualified_name;
  unsigned long long base_offset = 0;
  long long this_adjust = 0;
  bool has_result_adjust = false;
  long long result_adjust = 0;
  bool result_adjust_virtual = false;
  long long result_vcall_offset = 0;
  long long vcall_offset = 0;
};

struct AbiFunctionRecord
{
  AbiFunctionRecordKind kind = ABI_FUNCTION_RECORD_PARAMETER;
  std::string name;
  std::string substitution;
  std::string complete_substitution;
  std::string standard_substitution;
  bool standard_substitution_includes_arguments = false;
  std::string context_ref;
  std::string source_name;
  std::string discriminator;
  std::string terminal;
  std::string literal_suffix;
  AbiType type;
  std::vector<AbiType> types;
  std::vector<std::string> argument_refs;
  std::vector<std::string> namespace_qualifiers;
  std::vector<AbiFunctionQualifier> qualifiers;
};

struct AbiFunctionShape
{
  AbiFunctionTarget target;
  std::vector<AbiFunctionRecord> records;
};

struct AbiFactRecord
{
  AbiFactRecordKind kind = ABI_FACT_RECORD_TARGET;
  AbiDefinitionRecord definition;
  AbiTargetRecord target;
  AbiFunctionRecord function;
};

struct AbiFactCase
{
  std::string label;
  std::vector<AbiFactRecord> records;
};

struct AbiFactFile
{
  std::vector<AbiFactCase> cases;
};

struct AbiDefinitionTable
{
  std::map<std::string, AbiDefinitionRecord> definitions;

  void add(const AbiDefinitionRecord & definition)
  {
    if(!definitions.insert(std::make_pair(definition.id, definition)).second) {
      throw std::logic_error("duplicate ABI definition '" + definition.id + "'");
    }
  }

  const AbiDefinitionRecord * find(const std::string & id) const
  {
    const std::map<std::string, AbiDefinitionRecord>::const_iterator it =
      definitions.find(id);
    return it == definitions.end() ? 0 : &it->second;
  }
};

AbiFactRecord parse_fact_record_words(const std::vector<std::string> & words);
AbiFactFile parse_fact_text(const std::string & text);
std::string serialize_fact_file(const AbiFactFile & file);
std::string mangle_fact_case(const AbiFactCase & fact_case);
std::string mangle_target(const AbiTargetRecord & target,
                          const AbiDefinitionTable & definitions);
std::string mangle_fact_files(const std::vector<std::string> & input_paths);

}  // namespace abi_mangle
