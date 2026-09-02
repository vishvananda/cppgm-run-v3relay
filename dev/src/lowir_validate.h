#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "lowir_model.h"

// Program-wide facts published by validation and consumed by emission.  The
// symbol table is the one authority for top-level names: it records what kind
// of item a `@name` is and where that item lives in the parsed program.
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

    bool global() const
    {
      return kind == DECL_GLOBAL || kind == DEF_GLOBAL;
    }

    bool function() const
    {
      return kind == DECL_FUNCTION || kind == DEF_FUNCTION;
    }
  };

  std::unordered_map<std::string, SymbolRef> symbols;
  int entry = -1;
  int init = -1;
  int fini = -1;
  bool uses_eh = false;

  const SymbolRef * find(const std::string & name) const
  {
    const std::unordered_map<std::string, SymbolRef>::const_iterator found =
      symbols.find(name);
    return found == symbols.end() ? 0 : &found->second;
  }

  bool is_function(const std::string & name) const
  {
    const SymbolRef * ref = find(name);
    return ref != 0 && ref->function();
  }

  bool is_global(const std::string & name) const
  {
    const SymbolRef * ref = find(name);
    return ref != 0 && ref->global();
  }

  const lowir_model::Function * function_definition(const lowir_model::Program & program,
                                                    const std::string & name) const
  {
    const SymbolRef * ref = find(name);
    if(ref == 0 || ref->kind != SymbolRef::DEF_FUNCTION) return 0;
    return &program.functions[ref->index];
  }

  const lowir_model::FunctionDeclaration * function_declaration(
    const lowir_model::Program & program, const std::string & name) const
  {
    const SymbolRef * ref = find(name);
    if(ref == 0 || ref->kind != SymbolRef::DECL_FUNCTION) return 0;
    return &program.function_declarations[ref->index];
  }

  // The parameter list a call to `name` binds against: the definition's when
  // one exists, else the declaration's, else none.
  const std::vector<lowir_model::Parameter> * callee_parameters(
    const lowir_model::Program & program, const std::string & name) const
  {
    const SymbolRef * ref = find(name);
    if(ref == 0) return 0;
    if(ref->kind == SymbolRef::DEF_FUNCTION) return &program.functions[ref->index].params;
    if(ref->kind == SymbolRef::DECL_FUNCTION) {
      return &program.function_declarations[ref->index].params;
    }
    return 0;
  }
};

LowirProgramFacts ValidateLowirProgram(const lowir_model::Program & program);
