#pragma once

// Typed ABI fact model shared by the abimangle fact reader and the Itanium
// name encoder.  Later compiler stages build these records directly and call
// mangle_target; the line-oriented fact text is only the standalone tool's
// input form and is parsed once into this model.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace abi_mangle {

// Deepest nesting the reader accepts inside one type spelling and the encoder
// follows through definition references before rejecting the facts.  Every
// recursive path in the reader and the encoder is bounded by this constant, so
// malformed or cyclic facts fail deterministically instead of overflowing the
// stack.
const std::size_t ABI_MAXIMUM_NESTING_DEPTH = 2048;

// Fixed vocabularies.  The reader resolves each fact word to its enum once and
// the encoder and serializer index the same tables by enum.

enum AbiBuiltinType
{
  ABI_BUILTIN_VOID,
  ABI_BUILTIN_BOOL,
  ABI_BUILTIN_CHAR,
  ABI_BUILTIN_SCHAR,
  ABI_BUILTIN_UCHAR,
  ABI_BUILTIN_SHORT,
  ABI_BUILTIN_USHORT,
  ABI_BUILTIN_INT,
  ABI_BUILTIN_UINT,
  ABI_BUILTIN_LONG,
  ABI_BUILTIN_ULONG,
  ABI_BUILTIN_LONGLONG,
  ABI_BUILTIN_ULONGLONG,
  ABI_BUILTIN_INT128,
  ABI_BUILTIN_UINT128,
  ABI_BUILTIN_FLOAT,
  ABI_BUILTIN_DOUBLE,
  ABI_BUILTIN_LONGDOUBLE,
  ABI_BUILTIN_FLOAT128,
  ABI_BUILTIN_WCHAR,
  ABI_BUILTIN_CHAR16,
  ABI_BUILTIN_CHAR32,
  ABI_BUILTIN_NULLPTR,
  ABI_BUILTIN_AUTO,
  ABI_BUILTIN_COMPLEX_FLOAT,
  ABI_BUILTIN_COMPLEX_DOUBLE,
  ABI_BUILTIN_COMPLEX_LONGDOUBLE,
  ABI_BUILTIN_COUNT
};

struct AbiBuiltinTypeInfo
{
  const char * word;   // normalized fact spelling
  const char * code;   // Itanium <builtin-type>
  unsigned bits;       // value width of an integral type, 0 otherwise
  bool is_unsigned;
};

const AbiBuiltinTypeInfo & builtin_type_info(AbiBuiltinType type);
bool lookup_builtin_type(const std::string & word, AbiBuiltinType * type);

enum AbiSpecialFunctionKind
{
  ABI_SPECIAL_CONSTRUCTOR_COMPLETE,
  ABI_SPECIAL_CONSTRUCTOR_BASE,
  ABI_SPECIAL_CONSTRUCTOR_ALLOCATING,
  ABI_SPECIAL_DESTRUCTOR_DELETING,
  ABI_SPECIAL_DESTRUCTOR_COMPLETE,
  ABI_SPECIAL_DESTRUCTOR_BASE,
  ABI_SPECIAL_FUNCTION_COUNT
};

struct AbiSpecialFunctionInfo
{
  const char * word;
  const char * code;   // Itanium <ctor-dtor-name>
};

const AbiSpecialFunctionInfo & special_function_info(AbiSpecialFunctionKind kind);
bool lookup_special_function(const std::string & word,
                             AbiSpecialFunctionKind * kind);

enum AbiOperatorKind
{
  ABI_OPERATOR_NEW,
  ABI_OPERATOR_NEW_ARRAY,
  ABI_OPERATOR_DELETE,
  ABI_OPERATOR_DELETE_ARRAY,
  ABI_OPERATOR_PLUS,          // unary or binary by shape
  ABI_OPERATOR_MINUS,         // unary or binary by shape
  ABI_OPERATOR_ADDRESS_OF,    // unary address-of or binary bit-and by shape
  ABI_OPERATOR_DEREF,         // unary dereference or binary multiply by shape
  ABI_OPERATOR_UNARY_PLUS,
  ABI_OPERATOR_BINARY_PLUS,
  ABI_OPERATOR_UNARY_MINUS,
  ABI_OPERATOR_BINARY_MINUS,
  ABI_OPERATOR_BIT_AND,
  ABI_OPERATOR_MULTIPLY,
  ABI_OPERATOR_DIVIDE,
  ABI_OPERATOR_REMAINDER,
  ABI_OPERATOR_BIT_OR,
  ABI_OPERATOR_BIT_XOR,
  ABI_OPERATOR_ASSIGN,
  ABI_OPERATOR_PLUS_ASSIGN,
  ABI_OPERATOR_MINUS_ASSIGN,
  ABI_OPERATOR_MULTIPLY_ASSIGN,
  ABI_OPERATOR_DIVIDE_ASSIGN,
  ABI_OPERATOR_REMAINDER_ASSIGN,
  ABI_OPERATOR_BIT_AND_ASSIGN,
  ABI_OPERATOR_BIT_OR_ASSIGN,
  ABI_OPERATOR_BIT_XOR_ASSIGN,
  ABI_OPERATOR_SHIFT_LEFT,
  ABI_OPERATOR_SHIFT_RIGHT,
  ABI_OPERATOR_SHIFT_LEFT_ASSIGN,
  ABI_OPERATOR_SHIFT_RIGHT_ASSIGN,
  ABI_OPERATOR_EQUAL,
  ABI_OPERATOR_NOT_EQUAL,
  ABI_OPERATOR_LESS,
  ABI_OPERATOR_GREATER,
  ABI_OPERATOR_LESS_EQUAL,
  ABI_OPERATOR_GREATER_EQUAL,
  ABI_OPERATOR_SPACESHIP,
  ABI_OPERATOR_LOGICAL_NOT,
  ABI_OPERATOR_LOGICAL_AND,
  ABI_OPERATOR_LOGICAL_OR,
  ABI_OPERATOR_COMPLEMENT,
  ABI_OPERATOR_INCREMENT,
  ABI_OPERATOR_DECREMENT,
  ABI_OPERATOR_COMMA,
  ABI_OPERATOR_MEMBER_POINTER,
  ABI_OPERATOR_ARROW,
  ABI_OPERATOR_CALL,
  ABI_OPERATOR_INDEX,
  ABI_OPERATOR_CO_AWAIT,
  ABI_OPERATOR_COUNT
};

struct AbiOperatorInfo
{
  const char * word;         // canonical fact spelling
  const char * code;         // Itanium <operator-name>, binary form when shaped
  const char * unary_code;   // unary form for shape-dependent operators, or 0
};

const AbiOperatorInfo & operator_info(AbiOperatorKind kind);
bool lookup_operator(const std::string & word, AbiOperatorKind * kind);

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
  ABI_TYPE_NAME_OR_REFERENCE,   // definition id or bare qualified class name
  ABI_TYPE_NAMED,
  ABI_TYPE_BUILTIN,
  ABI_TYPE_TEMPLATE_PARAMETER,
  ABI_TYPE_POINTER,
  ABI_TYPE_LVALUE_REFERENCE,
  ABI_TYPE_RVALUE_REFERENCE,
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
  ABI_FUNCTION_RECORD_TERMINAL,
  ABI_FUNCTION_RECORD_VARIADIC,
  ABI_FUNCTION_RECORD_ABI_TAG,
  ABI_FUNCTION_RECORD_QUALIFIER,
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

enum AbiTerminalKind
{
  ABI_TERMINAL_SOURCE,             // ordinary unqualified source name
  ABI_TERMINAL_SPECIAL,            // constructor or destructor variant
  ABI_TERMINAL_OPERATOR,
  ABI_TERMINAL_LITERAL_OPERATOR,   // name holds the unencoded suffix
  ABI_TERMINAL_CONVERSION          // the owning record's type is the target
};

struct AbiTerminal
{
  AbiTerminalKind kind = ABI_TERMINAL_SOURCE;
  AbiSpecialFunctionKind special_function = ABI_SPECIAL_CONSTRUCTOR_COMPLETE;
  AbiOperatorKind operator_kind = ABI_OPERATOR_CALL;
  std::string name;
};

struct AbiArrayBound
{
  AbiArrayBoundKind kind = ABI_ARRAY_BOUND_VALUE;
  std::string value;
};

struct AbiType
{
  AbiTypeKind kind = ABI_TYPE_NAME_OR_REFERENCE;
  AbiBuiltinType builtin = ABI_BUILTIN_VOID;
  std::string name;
  std::string standard_substitution;
  std::string expression_ref;
  std::string context_ref;
  std::string discriminator;
  AbiArrayBound array_bound;
  std::size_t index = 0;
  bool is_const = false;
  bool is_volatile = false;
  bool variadic = false;
  bool substitutable = false;
  bool standard_substitution_includes_arguments = false;
  std::vector<AbiType> types;
  std::vector<std::string> argument_refs;
  std::vector<std::string> namespace_qualifiers;
  std::vector<std::string> abi_tags;
};

struct AbiTemplateArgument
{
  AbiTemplateArgumentKind kind = ABI_TEMPLATE_ARGUMENT_TYPE;
  AbiType type;           // type argument, dependent-value parameter type,
                          // or member-template owner
  AbiType value_type;
  AbiType owner_type;
  std::string name;
  std::string substitution;
  std::string entity_ref;
  std::string expression_ref;
  std::string symbol;
  long long value = 0;
  std::size_t index = 0;
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
  std::string text;
  std::string op;
  std::string entity_ref;
  long long value = 0;
  std::size_t index = 0;
  bool close_member_owner = false;
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
  AbiTerminal terminal;   // local, lambda, and namespace-lambda forms
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

struct AbiTargetRecord
{
  AbiTargetFactKind kind = ABI_TARGET_FACT_TYPE;
  bool c_linkage = false;
  bool internal_linkage = false;
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
  AbiTerminal terminal;
  AbiType type;           // parameter, result, or conversion target
  std::vector<AbiType> types;
  std::vector<std::string> argument_refs;
  std::vector<std::string> namespace_qualifiers;
  std::vector<AbiFunctionQualifier> qualifiers;
};

struct AbiTypeDefinition
{
  std::string id;
  AbiType type;
};

struct AbiArgumentDefinition
{
  std::string id;
  AbiTemplateArgument argument;
};

struct AbiExpressionDefinition
{
  std::string id;
  AbiDependentExpression expression;
};

struct AbiContextDefinition
{
  std::string id;
  AbiLocalContext context;
};

struct AbiEntityDefinition
{
  std::string id;
  AbiEntityFact entity;
};

// One ABI name: its file-local definitions, one target, and the function
// records that follow an encoding-form target.
struct AbiFactCase
{
  std::string label;
  std::vector<AbiTypeDefinition> types;
  std::vector<AbiArgumentDefinition> arguments;
  std::vector<AbiExpressionDefinition> expressions;
  std::vector<AbiContextDefinition> contexts;
  std::vector<AbiEntityDefinition> entities;
  bool has_target = false;
  AbiTargetRecord target;
  std::vector<AbiFunctionRecord> function_records;
};

struct AbiFactFile
{
  std::vector<AbiFactCase> cases;
};

// Index from definition id to the typed fact it names.  The table does not
// own the facts; the case (or a later stage's own storage) does.
class AbiDefinitionTable
{
public:
  void add_type(const std::string & id, const AbiType & type);
  void add_argument(const std::string & id, const AbiTemplateArgument & argument);
  void add_expression(const std::string & id,
                      const AbiDependentExpression & expression);
  void add_context(const std::string & id, const AbiLocalContext & context);
  void add_entity(const std::string & id, const AbiEntityFact & entity);
  void add_case(const AbiFactCase & fact_case);

  const AbiType * find_type(const std::string & id) const;
  const AbiTemplateArgument * find_argument(const std::string & id) const;
  const AbiDependentExpression * find_expression(const std::string & id) const;
  const AbiLocalContext * find_context(const std::string & id) const;
  const AbiEntityFact * find_entity(const std::string & id) const;
  std::size_t size() const { return entries_.size(); }

private:
  struct Entry
  {
    AbiDefinitionKind kind;
    const void * fact;
  };

  void add(const std::string & id, AbiDefinitionKind kind, const void * fact);
  const void * find(const std::string & id, AbiDefinitionKind kind) const;

  std::map<std::string, Entry> entries_;
};

// Fact reader and serializer (tool boundary).
void parse_fact_record_words(const std::vector<std::string> & words,
                             AbiFactCase * fact_case);
AbiFactFile parse_fact_text(const std::string & text);
std::string serialize_fact_file(const AbiFactFile & file);

// Encoder entry points.  mangle_target is the direct entry for later stages:
// function_records carry the encoding-form components and are empty for
// every other target form.
std::string mangle_target(const AbiTargetRecord & target,
                          const std::vector<AbiFunctionRecord> & function_records,
                          const AbiDefinitionTable & definitions);
std::string mangle_fact_case(const AbiFactCase & fact_case);
std::string mangle_fact_files(const std::vector<std::string> & input_paths);

}  // namespace abi_mangle
