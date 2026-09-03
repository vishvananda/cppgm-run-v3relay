// Complete-class contexts (9.2p2) and the lifetime facts a class fixes when
// its body closes: member bodies, constructor initializers, default member
// initializers, and the special members its subobjects demand.
#include "sema/scope_builder.h"

#include <set>
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

  ClassEntity& owner = model_.ClassAt(member_class);
  ClassField field;
  field.type = declared_type;
  field.access = member_access_;
  field.bit_width = static_cast<unsigned>(width);
  field.requested_alignment = AlignmentSpecifiers(node, scope);
  field.initializer = 0;
  field.static_member = false;
  field.binding = 0;
  if (!name.empty())
  {
    const BindingId binding = model_.AddBinding(
        scope, name, BINDING_VARIABLE, declared_type);
    Binding& value = model_.BindingAt(binding);
    value.access = member_access_;
    value.field_index = owner.fields.size();
    field.binding = binding;
  }
  owner.fields.push_back(field);
}

// Runs once per class body, over the definitions deferred while it was
// open.  Nested classes completed their own entries already, so the range
// is exactly this class's members and hidden friends.  Phase order keeps
// the source-shaped semantic tree: a constructor's mem-initializers precede
// its body under one function node, and default member initializers are
// detached nodes owned by the constructor entity.
void ScopeBuilder::CompleteClassMembers(ClassEntityId entity,
                                        std::size_t first_pending)
{
  const std::size_t last_pending = deferred_member_bodies_.size();
  if (EmitsSemantics())
  {
    {
      const ClassEntity& owner = model_.ClassAt(entity);
      if (owner.destructor != 0 &&
          !model_.FunctionAt(owner.destructor).deleted)
        EnsureSubobjectDestructors(entity);
    }
    for (std::size_t i = first_pending; i < last_pending; ++i)
    {
      const DeferredMemberBody pending = deferred_member_bodies_[i];
      if (pending.function == 0 || pending.function_node == 0)
        continue;
      const FunctionEntity& function = model_.FunctionAt(pending.function);
      if (function.special_member != SPECIAL_MEMBER_CONSTRUCTOR ||
          function.deleted || function.member_class != entity)
        continue;
      if (function.ctor_initializer != 0)
        BuildMemberInitializers(function.ctor_initializer, pending.scope,
                                pending.function_node, pending.function);
      EnsureSubobjectConstructors(entity, pending.function_node);
    }
  }
  for (std::size_t i = first_pending; i < last_pending; ++i)
  {
    // A body may define a local class, which pushes and pops its own
    // entries: copy before analyzing.
    const DeferredMemberBody pending = deferred_member_bodies_[i];
    if (pending.function == 0)
      continue;
    BuildFunctionBody(pending.body, pending.scope, pending.function,
                      pending.function_node);
  }
  if (EmitsSemantics())
  {
    for (std::size_t i = first_pending; i < last_pending; ++i)
    {
      const DeferredMemberBody pending = deferred_member_bodies_[i];
      if (pending.function == 0 || pending.function_node == 0)
        continue;
      const FunctionEntity& function = model_.FunctionAt(pending.function);
      if (function.special_member == SPECIAL_MEMBER_CONSTRUCTOR &&
          !function.deleted && function.member_class == entity)
        BuildConstructorDefaults(pending.function, pending.function_node,
                                 pending.scope);
    }
  }
  deferred_member_bodies_.resize(first_pending);
}

// Default member initializers are complete-class-context expressions
// (9.2p2): a constructor declared before a later field still sees that
// field.  The analyzed nodes stay detached from the source-shaped semantic
// tree and are owned by the constructor entity.
void ScopeBuilder::BuildConstructorDefaults(FunctionEntityId constructor,
                                            SemaId function_node,
                                            ScopeId function_scope)
{
  if (!EmitsSemantics() || function_node == 0 || function_scope == 0)
    return;
  const ClassEntityId entity = model_.FunctionAt(constructor).member_class;
  const std::vector<ClassField> fields = model_.ClassAt(entity).fields;
  std::vector<std::pair<BindingId, std::size_t> > defaults;
  for (std::size_t field_index = 0; field_index < fields.size();
       ++field_index)
  {
    const ClassField& field = fields[field_index];
    if (field.static_member || field.initializer == 0)
      continue;
    AstId source = field.initializer;
    if (arena_.At(source).kind == AST_INITIALIZER &&
        arena_.At(source).children.size() == 1)
      source = arena_.At(source).children[0];
    SemaId value = 0;
    const TypeId field_unqualified = types_.Unqualified(field.type);
    if (types_.Kind(field_unqualified) == TYPE_CLASS &&
        arena_.At(source).kind == AST_CALL_EXPRESSION &&
        !model_.ClassAt(types_.At(field_unqualified).entity).aggregate)
    {
      const std::vector<AstId>& call_children = arena_.At(source).children;
      if (call_children.size() != 2)
        throw std::runtime_error("invalid default member constructor");
      const std::vector<AstId>& argument_nodes =
          arena_.At(call_children[1]).children;
      std::vector<SemaId> arguments;
      for (std::size_t argument = 0; argument < argument_nodes.size();
           ++argument)
        arguments.push_back(expression_.Analyze(argument_nodes[argument],
                                                function_scope));
      const FunctionEntityId target_constructor = ResolveConstructor(
          field.type, arguments, function_scope);
      const FunctionEntity& target = model_.FunctionAt(target_constructor);
      const TypeNode& constructor_type = types_.At(types_.Unqualified(
          target.type));
      std::vector<SemaId> converted;
      for (std::size_t argument = 0; argument < arguments.size(); ++argument)
        converted.push_back(expression_.Initialize(
            arguments[argument], constructor_type.parameters[argument + 1]));
      for (std::size_t parameter = arguments.size() + 1;
           parameter < constructor_type.parameters.size(); ++parameter)
      {
        if (parameter >= target.default_arguments.size() ||
            target.default_arguments[parameter] == 0)
          throw std::runtime_error(
              "missing default member constructor argument");
        converted.push_back(expression_.AnalyzeInitializer(
            target.default_arguments[parameter], target.scope,
            constructor_type.parameters[parameter]));
      }
      value = tree_->Make(SEMA_MEMBER_INITIALIZER);
      SemaNode& member = tree_->At(value);
      member.scope = function_scope;
      member.type = field.type;
      member.binding = field.binding;
      member.function = target_constructor;
      member.category = VC_PRVALUE;
      for (std::size_t argument = 0; argument < converted.size(); ++argument)
        tree_->Append(value, converted[argument]);
    }
    else
      value = expression_.AnalyzeInitializer(field.initializer,
                                             function_scope, field.type);
    defaults.push_back(std::make_pair(field.binding, value));
  }
  model_.FunctionAt(constructor).default_member_initializers.swap(defaults);
}

// 12.6.2p8: a constructor default-initializes every direct subobject that
// neither its mem-initializer-list nor a default member initializer names.
// That makes those subobjects' default constructors odr-used by this
// definition, so their entities must exist before lowering asks for them.
void ScopeBuilder::EnsureSubobjectConstructors(ClassEntityId entity,
                                               SemaId function_node)
{
  std::set<ClassEntityId> initialized_bases;
  std::set<BindingId> initialized_fields;
  for (SemaId child = tree_->At(function_node).first_child; child != 0;
       child = tree_->At(child).next_sibling)
  {
    const SemaNode& node = tree_->At(child);
    if (node.kind != SEMA_MEMBER_INITIALIZER)
      continue;
    if (node.binding != 0)
      initialized_fields.insert(node.binding);
    else if (node.function != 0)
      initialized_bases.insert(model_.FunctionAt(node.function).member_class);
  }
  const std::vector<ClassBase> bases = model_.ClassAt(entity).bases;
  const std::vector<ClassField> fields = model_.ClassAt(entity).fields;
  for (std::size_t i = 0; i < bases.size(); ++i)
  {
    if (initialized_bases.count(bases[i].entity) != 0)
      continue;
    const ClassEntity& base = model_.ClassAt(bases[i].entity);
    if (!base.trivial_default_constructor)
      (void)EnsureDefaultConstructor(base.type);
  }
  for (std::size_t i = 0; i < fields.size(); ++i)
  {
    const ClassField& field = fields[i];
    if (field.static_member || field.binding == 0 || field.initializer != 0 ||
        initialized_fields.count(field.binding) != 0)
      continue;
    TypeId element = types_.Unqualified(field.type);
    while (types_.Kind(element) == TYPE_ARRAY)
      element = types_.Unqualified(types_.At(element).base);
    if (types_.Kind(element) == TYPE_CLASS &&
        !model_.ClassAt(types_.At(element).entity).trivial_default_constructor)
      (void)EnsureDefaultConstructor(element);
  }
}

// 12.4p8: a destructor destroys every direct member and base with a
// non-trivial destructor, whether the destructor is written or synthesized.
void ScopeBuilder::EnsureSubobjectDestructors(ClassEntityId entity)
{
  const std::vector<ClassBase> bases = model_.ClassAt(entity).bases;
  const std::vector<ClassField> fields = model_.ClassAt(entity).fields;
  for (std::size_t i = 0; i < bases.size(); ++i)
  {
    const ClassEntity& base = model_.ClassAt(bases[i].entity);
    if (!base.trivial_destructor)
      (void)EnsureDestructor(base.type);
  }
  for (std::size_t i = 0; i < fields.size(); ++i)
  {
    if (fields[i].static_member || fields[i].binding == 0)
      continue;
    TypeId element = types_.Unqualified(fields[i].type);
    while (types_.Kind(element) == TYPE_ARRAY)
      element = types_.Unqualified(types_.At(element).base);
    if (types_.Kind(element) == TYPE_CLASS &&
        !model_.ClassAt(types_.At(element).entity).trivial_destructor)
      (void)EnsureDestructor(element);
  }
}
