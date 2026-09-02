#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "lowir_model.h"

struct LowirProgramFacts
{
  struct SymbolRef
  {
    enum Kind
    {
      DECL_GLOBAL,
      DEF_GLOBAL,
      DECL_FUNCTION,
      DEF_FUNCTION
    } kind = DECL_GLOBAL;

    std::size_t index = 0;
  };

  std::unordered_map<std::string, SymbolRef> symbols;
  int entry = -1;
  int init = -1;
  int fini = -1;
  bool uses_eh = false;
  std::size_t global_declaration_count = 0;
  std::size_t global_definition_count = 0;
  std::size_t function_declaration_count = 0;
  std::size_t function_definition_count = 0;
  std::size_t alias_count = 0;
  std::string entry_function;
};

LowirProgramFacts ValidateLowirProgram(const lowir_model::Program & program);
