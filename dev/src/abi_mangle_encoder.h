#pragma once

// Internal encoder state for one Itanium mangled name.

#include "abi_mangle.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace abi_mangle {

// Identity of one ABI component within a mangled name.  Structurally equal
// components intern to the same id, so substitution equality is an integer
// compare and the key spelling of a component is bounded by its fan-out, not
// by the size of its subtree.
typedef std::size_t AbiKeyId;   // 0 = not a component

// Components of a qualified name (a leading :: is ignored) and whether the
// name needs the N...E wrapper (more than one component, except std::X).
std::vector<std::string> name_components(const std::string & name);
bool needs_nested_name(const std::vector<std::string> & components);

class KeyInterner
{
public:
  AbiKeyId intern(const std::string & spelling);

private:
  std::unordered_map<std::string, AbiKeyId> ids_;
};

// Itanium substitution table: one slot per registered component key in
// registration order, spelled S_, S0_, S1_, ...
class SubstitutionTable
{
public:
  bool lookup(AbiKeyId key, std::string * spelling) const;
  void add(AbiKeyId key);

private:
  std::vector<std::size_t> slots_;   // indexed by key id; slot number + 1
  std::size_t count_ = 0;
};

// A resolved type: the node reached through definition aliases plus the cv
// qualifiers accumulated on the way.  A NAME_OR_REFERENCE node that names no
// definition denotes the class with that qualified name.
struct ResolvedType
{
  const AbiType * node = 0;
  bool is_const = false;
  bool is_volatile = false;
};

// One component of a function name being emitted.
struct NamePiece
{
  enum Kind { SOURCE, TEMPLATE };
  Kind kind = SOURCE;
  const std::string * spelling = 0;                 // unqualified source name
  AbiKeyId key = 0;                                 // prefix / template-name key
  AbiKeyId complete_key = 0;                        // specialization key
  const std::string * standard_substitution = 0;    // e.g. So, or 0
  bool standard_substitution_includes_arguments = false;
  const std::vector<std::string> * argument_refs = 0;
};

// Everything one function encoding needs, lowered from any target form.
struct FunctionFacts
{
  std::vector<NamePiece> pieces;
  bool standard = false;
  std::string standard_substitution;
  const AbiFunctionRecord * context = 0;
  const AbiTerminal * terminal = 0;
  const AbiType * conversion_type = 0;
  std::vector<AbiFunctionQualifier> qualifiers;
  std::vector<std::string> tags;
  std::vector<AbiKeyId> template_prefix_keys;
  std::vector<std::string> template_arguments;
  bool template_encoding = false;
  std::vector<const AbiType *> parameters;
  std::vector<const AbiType *> results;
  bool variadic = false;
};

class Mangler
{
public:
  Mangler(const AbiDefinitionTable & definitions, std::size_t depth);

  std::string mangle_target(const AbiTargetRecord & target,
                            const std::vector<AbiFunctionRecord> & records);
  std::string mangle_type(const AbiType & type);
  std::string mangle_template_arg(const AbiTemplateArgument & argument);
  std::string mangle_expression(const AbiDependentExpression & expression);

private:
  friend class DepthScope;

  struct KeyCacheEntry
  {
    AbiKeyId key = 0;
    bool in_progress = false;
  };

  // names
  AbiKeyId name_key(const std::string & qualified_name,
                    const std::vector<std::string> & tags);
  AbiKeyId prefix_key(const std::vector<std::string> & components,
                      std::size_t count);
  void append_prefixes(const std::vector<std::string> & components,
                       std::size_t count, std::string * out);
  std::string mangle_name_body(const std::string & qualified_name,
                               const std::vector<std::string> & tags,
                               AbiKeyId register_key, bool * nested);
  std::string mangle_qualified_name(const std::string & qualified_name);
  std::string mangle_internal_name(const std::string & qualified_name);
  std::string mangle_tag_list(const std::vector<std::string> & tags) const;

  // functions
  std::string mangle_function(const AbiFunctionTarget & target,
                              const std::vector<AbiFunctionRecord> & records);
  void lower_path_target(const AbiFunctionTarget & target,
                         const std::vector<AbiFunctionRecord> & records,
                         FunctionFacts * facts,
                         std::vector<std::string> * component_storage);
  void lower_records(const std::vector<AbiFunctionRecord> & records,
                     FunctionFacts * facts);
  std::string mangle_function_facts(const FunctionFacts & facts);
  std::string mangle_function_name(const FunctionFacts & facts);
  std::string mangle_context_function_name(const FunctionFacts & facts);
  void append_name_piece(const NamePiece & piece, bool prefix,
                         std::string * out);
  std::string mangle_terminal(const FunctionFacts & facts, bool owned);
  std::string mangle_context(const std::string & ref);
  std::string mangle_special_target(const AbiTargetRecord & target);
  std::string mangle_thunk(const AbiTargetRecord & target,
                           const std::string & encoding) const;
  std::string mangle_entity_encoding(const AbiEntityFact & entity);
  std::string mangle_entity_impl(const AbiEntityFact & entity);

  // types
  ResolvedType resolve_type(const AbiType & input) const;
  std::string mangle_type_impl(const AbiType & input, bool ignore_cv);
  std::string mangle_node(const AbiType & node, const std::string & class_name);
  std::string mangle_named_type(const std::string & qualified_name,
                                const std::vector<std::string> & tags);
  std::string mangle_template_body(const AbiType & node, bool * nested);
  std::string mangle_member_type(const AbiType & node);
  std::string mangle_owner_body(const AbiType & input, std::string * context);
  std::string mangle_local_body(const AbiType & node, std::string * context);
  std::string mangle_template_args(const std::vector<std::string> & refs);
  std::string mangle_argument_ref(const std::string & ref);
  std::string mangle_integral_value(const AbiType & type, long long value) const;
  std::string mangle_expression_impl(const AbiDependentExpression & expression);
  std::string mangle_expression_ref(const std::string & ref);

  // keys
  AbiKeyId key_of_type(const AbiType & input, bool ignore_cv);
  AbiKeyId key_of_node(const AbiType & node);
  AbiKeyId key_of_node_impl(const AbiType & node);
  AbiKeyId local_type_key(const std::string & context_ref,
                          const std::string & name,
                          const std::string & discriminator);
  AbiKeyId lambda_closure_key(const std::string & context_ref,
                              const std::string & discriminator,
                              const std::vector<AbiType> & signature);
  AbiKeyId namespace_lambda_key(const std::vector<std::string> & qualifiers,
                                const std::string & name);
  AbiKeyId key_of_argument(const AbiTemplateArgument & argument);
  AbiKeyId key_of_argument_ref(const std::string & ref);
  AbiKeyId key_of_expression(const AbiDependentExpression & expression);
  AbiKeyId key_of_expression_ref(const std::string & ref);
  AbiKeyId key_of_entity(const AbiEntityFact & entity);
  AbiKeyId cached_key(const void * fact, bool * ready);
  void store_key(const void * fact, AbiKeyId key);
  void release_key(const void * fact);

  const AbiType & type_definition(const std::string & ref) const;
  const AbiTemplateArgument & argument_definition(const std::string & ref) const;
  const AbiDependentExpression & expression_definition(
    const std::string & ref) const;
  const AbiEntityFact & entity_definition(const std::string & ref) const;
  const AbiLocalContext & context_definition(const std::string & ref) const;

  const AbiDefinitionTable & definitions_;
  KeyInterner keys_;
  SubstitutionTable substitutions_;
  std::unordered_map<const void *, KeyCacheEntry> key_cache_;
  std::size_t depth_;
};

// Bounds every recursive path in the encoder by ABI_MAXIMUM_NESTING_DEPTH.
class DepthScope
{
public:
  explicit DepthScope(Mangler * mangler);
  ~DepthScope();

private:
  Mangler * mangler_;
};

}  // namespace abi_mangle
