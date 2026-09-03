#include "sema/expr_sema.h"

#include <stdexcept>

FunctionEntityId ExpressionAnalyzer::BuiltinFunction(const std::string& name)
{
  const std::map<std::string, FunctionEntityId>::const_iterator found =
      builtin_functions_.find(name);
  if (found != builtin_functions_.end())
    return found->second;

  const TypeId void_type = types_.Fundamental(FT_VOID);
  const TypeId const_char = types_.Cv(types_.Fundamental(FT_CHAR), true);
  const TypeId void_pointer = types_.Pointer(void_type);
  const TypeId const_void_pointer = types_.Pointer(
      types_.Cv(void_type, true));
  const TypeId const_char_pointer = types_.Pointer(const_char);
  const TypeId size_type = types_.Fundamental(FT_UNSIGNED_LONG_INT);
  std::vector<TypeId> parameters;
  TypeId result = void_type;
  if (name == "__builtin_unreachable") {
    // no parameters
  } else if (name == "__builtin_strlen") {
    parameters.push_back(const_char_pointer);
    result = size_type;
  } else if (name == "__builtin_memcpy" ||
             name == "__builtin_memmove") {
    parameters.push_back(void_pointer);
    parameters.push_back(const_void_pointer);
    parameters.push_back(size_type);
    result = void_pointer;
  } else {
    throw std::runtime_error("unknown builtin function");
  }

  const FunctionEntityId function = model_.CreateFunction(
      model_.GlobalScope(), name, types_.Function(result, parameters));
  model_.FunctionAt(function).builtin = true;
  builtin_functions_[name] = function;
  return function;
}
