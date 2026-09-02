#pragma once

#include "abi_mangle.h"

#include <map>
#include <string>
#include <vector>

namespace abi_mangle {

class SubstitutionTable
{
public:
  bool lookup(const std::string & key, std::string * spelling) const;
  void add(const std::string & key);

private:
  std::string sequence_id(std::size_t index) const;

  std::vector<std::string> keys_;
  std::map<std::string, std::size_t> indexes_;
};

class Mangler
{
public:
  explicit Mangler(const AbiDefinitionTable & definitions);

  std::string mangle_type(const AbiType & type);
  std::string mangle_template_arg(const AbiTemplateArgument & argument);
  std::string mangle_expression(const AbiDependentExpression & expression);
  std::string mangle_entity_encoding(const AbiEntityFact & entity);
  std::string mangle_qualified_name(
    const std::string & qualified_name, bool register_last,
    const std::vector<std::string> & abi_tags = std::vector<std::string>());
  std::string mangle_prefix_chain(
    const std::string & qualified_name, bool register_last,
    const std::vector<std::string> & abi_tags = std::vector<std::string>());
  std::string mangle_template_args(const std::vector<std::string> & refs);
  std::string mangle_special_target(const AbiTargetRecord & target);
  std::string mangle_target(const AbiTargetRecord & target);
  std::string mangle_target(const AbiTargetRecord & target,
                            const AbiFunctionShape & shape);
  std::string mangle_function(const AbiFunctionShape & shape);

  std::string key_of_type(const AbiType & type);
  std::string key_of_argument(const AbiTemplateArgument & argument);
  std::string key_of_expression(const AbiDependentExpression & expression);
  std::string key_of_entity(const AbiEntityFact & entity);

private:
  std::string mangle_type_impl(const AbiType & type, std::size_t depth);
  std::string mangle_long_type_chain(const AbiType & type);
  std::string mangle_named_type(const AbiType & type, std::size_t depth);
  std::string mangle_template_type(const AbiType & type, std::size_t depth);
  std::string mangle_member_type(const AbiType & type, std::size_t depth);
  std::string mangle_owner_prefix(const AbiType & type, std::size_t depth);
  std::string mangle_template_name(const std::string & qualified_name);
  std::string mangle_template_member_name(const AbiType & type,
                                          std::size_t depth);
  std::string mangle_type_argument(const std::string & ref,
                                   std::size_t depth);
  std::string mangle_argument_ref(const std::string & ref,
                                  std::size_t depth);
  std::string mangle_expression_impl(const AbiDependentExpression & expression,
                                     std::size_t depth);
  std::string mangle_expression_ref(const std::string & ref,
                                    std::size_t depth);
  std::string mangle_entity_impl(const AbiEntityFact & entity,
                                 std::size_t depth);
  std::string mangle_internal_name(const std::string & qualified_name);
  std::string mangle_function_path(const AbiFunctionShape & shape);
  std::string mangle_function_encoding(const AbiFunctionShape & shape);
  std::string mangle_owned_function(const AbiFunctionShape & shape);
  std::string mangle_context(const std::string & ref);
  std::string mangle_local_discriminator(
    const std::string & discriminator) const;
  std::string mangle_context_function_name(
    const std::vector<AbiFunctionRecord> & records,
    std::vector<std::string> * template_arguments,
    bool * has_template_encoding);
  std::string mangle_path_name(
    const AbiFunctionTarget & target,
    const std::vector<AbiFunctionRecord> & records,
    const std::vector<std::string> & template_arguments);
  std::string mangle_function_name(
    const std::vector<AbiFunctionRecord> & records,
    std::vector<std::string> * template_arguments,
    bool * has_template_encoding);
  std::string mangle_function_terminal(const AbiFunctionRecord & record);
  std::string mangle_call_offset(long long offset) const;

  std::string key_of_type_impl(const AbiType & type, std::size_t depth);
  std::string key_of_argument_impl(const AbiTemplateArgument & argument,
                                   std::size_t depth);
  std::string key_of_expression_impl(const AbiDependentExpression & expression,
                                     std::size_t depth);
  std::string key_of_entity_impl(const AbiEntityFact & entity,
                                 std::size_t depth);

  const AbiDefinitionRecord * definition(const std::string & id) const;
  const AbiType * type_definition(const std::string & id) const;
  const AbiTemplateArgument * argument_definition(const std::string & id) const;
  const AbiDependentExpression * expression_definition(
    const std::string & id) const;
  const AbiEntityFact * entity_definition(const std::string & id) const;

  std::string mangle_builtin(const AbiType & type) const;
  std::string builtin_code(const AbiType & type) const;
  std::string mangle_integral_value(const AbiType & type, long long value) const;
  std::string mangle_tag_list(const std::vector<std::string> & tags) const;
  std::string type_name_key(const AbiType & type) const;
  std::string argument_key_ref(const std::string & ref, std::size_t depth);
  std::string expression_key_ref(const std::string & ref, std::size_t depth);
  std::string type_key_ref(const std::string & ref, std::size_t depth);

  const AbiDefinitionTable & definitions_;
  SubstitutionTable substitutions_;
  std::map<std::string, bool> active_type_keys_;
  std::map<std::string, bool> active_type_mangles_;
  std::map<std::string, std::string> type_key_cache_;
  std::map<std::string, std::string> argument_key_cache_;
  std::map<std::string, std::string> expression_key_cache_;
  std::map<std::string, std::string> entity_key_cache_;
};

}  // namespace abi_mangle
