// Complete-class semantic work that depends on the collected member set.
#include "sema/scope_builder.h"

#include <stdexcept>

void ScopeBuilder::BuildBitFieldDeclaration(AstId node, ScopeId scope)
{
  ClassEntityId member_class = 0;
  if (!model_.ClassForScope(scope, member_class))
    throw std::runtime_error("bit-field is not declared in a class");
  const AstId specifiers = FindChild(node, AST_DECL_SPECIFIER_SEQ);
  const AstId bit_declarator = FindChild(node, AST_BIT_FIELD_DECLARATOR);
  if (specifiers == 0 || bit_declarator == 0 ||
      arena_.At(bit_declarator).children.empty() ||
      arena_.At(bit_declarator).children.size() > 2)
    throw std::runtime_error("invalid bit-field declaration");

  const TypeId declared_type = BuildSpecifierType(specifiers, scope);
  const TypeId unqualified = types_.Unqualified(declared_type);
  TypeId allocation_type = unqualified;
  if (types_.Kind(allocation_type) == TYPE_ENUM)
    allocation_type = types_.Unqualified(types_.At(allocation_type).base);
  if (types_.Kind(allocation_type) != TYPE_FUNDAMENTAL ||
      !FundamentalIsIntegral(types_.At(allocation_type).fundamental))
    throw std::runtime_error("bit-field type is not integral or enumeration");

  const std::vector<AstId>& bit_children =
      arena_.At(bit_declarator).children;
  const AstId declarator = bit_children.size() == 2 ? bit_children[0] : 0;
  const AstId width_expression = bit_children.back();
  const long long width = ConstantValue(width_expression, scope);
  const unsigned unit_bits = static_cast<unsigned>(
      FundamentalSize(types_.At(allocation_type).fundamental) * 8);
  if (width < 0 || static_cast<unsigned long long>(width) > unit_bits)
    throw std::runtime_error("bit-field width is outside its allocation unit");

  const AstId identifier = FindIdentifier(declarator);
  const std::string name = IdentifierName(identifier);
  if (width == 0 && !name.empty())
    throw std::runtime_error("named bit-field has zero width");

  ClassField field;
  field.type = declared_type;
  field.access = member_access_;
  field.bit_width = static_cast<unsigned>(width);
  field.initializer = 0;
  field.static_member = false;
  field.binding = 0;
  if (!name.empty())
  {
    const BindingId binding = model_.AddBinding(
        scope, name, BINDING_VARIABLE, declared_type);
    Binding& value = model_.BindingAt(binding);
    value.access = member_access_;
    value.bit_field = true;
    value.bit_width = static_cast<unsigned>(width);
    field.binding = binding;
  }
  model_.ClassAt(member_class).fields.push_back(field);
}

void ScopeBuilder::BuildCompletedMemberInitializers(ClassEntityId entity)
{
  if (!EmitsSemantics())
    return;
  const ClassEntity& owner = model_.ClassAt(entity);
  for (std::size_t constructor_index = 0;
       constructor_index < owner.constructors.size();
       ++constructor_index) {
    const FunctionEntityId constructor = owner.constructors[constructor_index];
    const FunctionEntity& function = model_.FunctionAt(constructor);
    if (function.ctor_initializer == 0 || function.deleted)
      continue;
    SemaId function_node = 0;
    for (std::size_t i = 0; i < deferred_semantics_.size(); ++i) {
      const SemaId candidate = deferred_semantics_[i];
      if (candidate != 0 && tree_->At(candidate).function == constructor &&
          tree_->At(candidate).kind == SEMA_FUNCTION_DEFINITION) {
        function_node = candidate;
        break;
      }
    }
    if (function_node == 0)
      continue;
    ScopeId function_scope = 0;
    for (SemaId child = tree_->At(function_node).first_child; child != 0;
         child = tree_->At(child).next_sibling) {
      if (tree_->At(child).kind == SEMA_PARAMETER) {
        function_scope = tree_->At(child).scope;
        break;
      }
      if (tree_->At(child).kind == SEMA_COMPOUND_STATEMENT &&
          function_scope == 0)
        function_scope = tree_->At(child).scope;
    }
    if (function_scope != 0)
      BuildMemberInitializers(function.ctor_initializer, function_scope,
                              function_node, constructor);
  }
}

void ScopeBuilder::BuildCompletedMemberBodies(ClassEntityId entity)
{
  for (std::size_t i = 0; i < deferred_member_bodies_.size(); ++i) {
    DeferredMemberBody& pending = deferred_member_bodies_[i];
    if (pending.built || pending.function == 0 || pending.body == 0 ||
        model_.FunctionAt(pending.function).member_class != entity)
      continue;
    pending.built = true;
    labels_.clear();
    gotos_.clear();
    initialized_locals_.clear();
    jump_sequence_ = 0;
    (void)BuildCompound(pending.body, pending.scope, pending.function, 0, 0,
                        pending.function_node);
    for (std::size_t g = 0; g < gotos_.size(); ++g) {
      const std::map<std::string, LabelRecord>::const_iterator label =
          labels_.find(gotos_[g].name);
      if (label == labels_.end())
        throw std::runtime_error("goto target does not name a label");
      if (gotos_[g].node != 0) {
        tree_->At(gotos_[g].node).has_value = true;
        tree_->At(gotos_[g].node).value = label->second.ordinal;
      }
      CheckJumpTarget(gotos_[g].sequence,
                      gotos_[g].node != 0 ? tree_->At(gotos_[g].node).scope :
                      pending.scope,
                      label->second.sequence, label->second.scope);
    }
    labels_.clear();
    gotos_.clear();
    initialized_locals_.clear();
  }
}
