#include "sema/expr_sema.h"

#include <stdexcept>

// One function entity per builtin kind and translation unit, created on
// first use with the signature the kind defines.
FunctionEntityId ExpressionAnalyzer::BuiltinFunction(BuiltinFunctionKind kind)
{
  if (kind <= BUILTIN_NONE || kind >= BUILTIN_KIND_COUNT)
    throw std::logic_error("unknown builtin function");
  if (builtin_functions_[kind] != 0)
    return builtin_functions_[kind];

  const TypeId void_type = types_.Fundamental(FT_VOID);
  const TypeId const_char = types_.Cv(types_.Fundamental(FT_CHAR), true);
  const TypeId void_pointer = types_.Pointer(void_type);
  const TypeId const_void_pointer = types_.Pointer(
      types_.Cv(void_type, true));
  const TypeId const_char_pointer = types_.Pointer(const_char);
  const TypeId size_type = types_.Fundamental(FT_UNSIGNED_LONG_INT);
  std::vector<TypeId> parameters;
  TypeId result = void_type;
  switch (kind)
  {
  case BUILTIN_UNREACHABLE:
    break;
  case BUILTIN_STRLEN:
    parameters.push_back(const_char_pointer);
    result = size_type;
    break;
  case BUILTIN_MEMCPY:
  case BUILTIN_MEMMOVE:
    parameters.push_back(void_pointer);
    parameters.push_back(const_void_pointer);
    parameters.push_back(size_type);
    result = void_pointer;
    break;
  default:
    break;
  }

  const FunctionEntityId function = model_.CreateFunction(
      model_.GlobalScope(), BuiltinSpelling(kind),
      types_.Function(result, parameters));
  model_.FunctionAt(function).builtin = kind;
  builtin_functions_[kind] = function;
  return function;
}
