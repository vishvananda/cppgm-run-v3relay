// Complete-class contexts (9.2p2) and the lifetime facts a class fixes when
// its body closes: member bodies, constructor initializers, default member
// initializers, and the special members its subobjects demand.
#include "sema/scope_builder.h"

#include <algorithm>
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
      if ((function.copy_constructor || function.move_constructor) &&
          (function.synthesized || function.defaulted))
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
          (!(function.copy_constructor || function.move_constructor) ||
           (!function.synthesized && !function.defaulted)) &&
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

namespace
{

std::size_t AlignUp(std::size_t value, std::size_t alignment)
{
  if (alignment <= 1)
    return value;
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + alignment - remainder;
}

bool LayoutKnown(const SemaModel& model, const TypeTable& types, TypeId type)
{
  const TypeNode& node = types.At(type);
  switch (node.kind)
  {
  case TYPE_CV: case TYPE_REFERENCE:
    return LayoutKnown(model, types, node.base);
  case TYPE_FUNDAMENTAL:
    return FundamentalSize(node.fundamental) != 0;
  case TYPE_POINTER: case TYPE_MEMBER_POINTER: case TYPE_ENUM:
    return true;
  case TYPE_ARRAY:
    return node.array_bound != 0 && LayoutKnown(model, types, node.base);
  case TYPE_CLASS:
    return model.ClassAt(node.entity).layout_complete;
  case TYPE_TEMPLATE_PARAM: case TYPE_FUNCTION: case TYPE_INVALID:
    return false;
  }
  return false;
}

}  // namespace

void ScopeBuilder::CompleteClassLayout(ClassEntityId entity)
{
  ClassEntity& value = model_.ClassAt(entity);
  if (value.layout_complete)
    return;

  // A primary class template is a layout pattern until its template
  // parameters are substituted.  Preserve the fields for lookup, but defer
  // sizeof/alignment and offset assignment while one of them is incomplete.
  for (std::size_t i = 0; i < value.fields.size(); ++i)
    if (!LayoutKnown(model_, types_, value.fields[i].type))
      return;

  std::size_t offset = 0;
  std::size_t alignment = 1;
  std::size_t size = 0;
  const std::size_t pack_alignment = value.pack_alignment;
  for (std::size_t i = 0; i < value.bases.size(); ++i)
  {
    const ClassBase& base = value.bases[i];
    const ClassEntity& base_entity = model_.ClassAt(base.entity);
    if (!base_entity.layout_complete)
      throw std::runtime_error("base class is incomplete");
    const std::size_t base_alignment = pack_alignment == 0 ?
        base_entity.alignment : std::min(base_entity.alignment, pack_alignment);
    const std::size_t base_size = base_entity.size;
    alignment = std::max(alignment, base_alignment);
    // Empty bases use offset zero; a same-type member reserves their size.
    if (base_entity.empty || value.is_union)
      value.bases[i].offset = 0;
    else
    {
      offset = AlignUp(offset, base_alignment);
      value.bases[i].offset = offset;
      offset += base_size;
    }
    size = std::max(size, value.is_union ? base_size : offset);
  }

  // A bit-field allocation unit is the storage-sized unit of its declared
  // type.  Adjacent fields share it only when the next field still fits;
  // changing type or crossing the unit boundary starts a new aligned unit.
  // `bit_offset` is measured from the low bit because the x86-64 LowIR model
  // represents the target's ordinary little-endian allocation convention.
  std::size_t bit_unit_offset = 0;
  std::size_t bit_unit_size = 0;
  unsigned bit_unit_bits = 0;
  unsigned bit_used = 0;
  bool have_bit_unit = false;
  for (std::size_t i = 0; i < value.fields.size(); ++i)
  {
    ClassField& field = value.fields[i];
    if (field.static_member)
      continue;
    const std::size_t natural_field_alignment = types_.AlignOf(field.type);
    if (field.requested_alignment != 0 &&
        field.requested_alignment < natural_field_alignment)
      throw std::runtime_error("member alignas weakens natural alignment");
    const std::size_t requested_field_alignment = std::max(
        natural_field_alignment, field.requested_alignment);
    const std::size_t field_alignment = pack_alignment == 0 ?
        requested_field_alignment :
        std::min(requested_field_alignment, pack_alignment);
    const std::size_t field_size = types_.SizeOf(field.type);
    alignment = std::max(alignment, field_alignment);
    if (field.bit_width != 0 || field.binding == 0)
    {
      const TypeId allocation_type = types_.Unqualified(field.type);
      const TypeId unit_type = types_.Kind(allocation_type) == TYPE_ENUM ?
          types_.Unqualified(types_.At(allocation_type).base) :
          allocation_type;
      const unsigned unit_bits = static_cast<unsigned>(
          FundamentalSize(types_.At(unit_type).fundamental) * 8);
      if (field.bit_width == 0)
      {
        // A zero-width unnamed field ends the current unit and forces the
        // following field to begin at the next unit boundary.
        if (have_bit_unit)
        {
          offset = std::max(offset, bit_unit_offset + bit_unit_size);
          have_bit_unit = false;
          bit_used = 0;
        }
        offset = AlignUp(offset, field_alignment);
        field.offset = value.is_union ? 0 : offset;
        field.bit_offset = 0;
        size = std::max(size, value.is_union ? field_size : offset);
        continue;
      }
      if (value.is_union)
      {
        field.offset = 0;
        field.bit_offset = 0;
        size = std::max(size, field_size);
        continue;
      }
      if (!have_bit_unit || bit_unit_size != field_size ||
          bit_used + field.bit_width > unit_bits)
      {
        if (have_bit_unit)
          offset = std::max(offset, bit_unit_offset + bit_unit_size);
        offset = AlignUp(offset, field_alignment);
        bit_unit_offset = offset;
        bit_unit_size = field_size;
        bit_unit_bits = unit_bits;
        bit_used = 0;
        have_bit_unit = true;
      }
      field.offset = bit_unit_offset;
      field.bit_offset = bit_used;
      bit_used += field.bit_width;
      size = std::max(size, bit_unit_offset + bit_unit_size);
      if (bit_used == bit_unit_bits)
      {
        offset = bit_unit_offset + bit_unit_size;
        have_bit_unit = false;
        bit_used = 0;
      }
      continue;
    }
    if (have_bit_unit)
    {
      offset = std::max(offset, bit_unit_offset + bit_unit_size);
      have_bit_unit = false;
      bit_used = 0;
    }
    if (value.is_union)
      field.offset = 0;
    else
    {
      offset = AlignUp(offset, field_alignment);
      for (std::size_t base = 0; base < value.bases.size(); ++base)
      {
        const ClassBase& base_info = value.bases[base];
        const ClassEntity& base_entity = model_.ClassAt(base_info.entity);
        if (base_entity.empty &&
            types_.Kind(types_.Unqualified(field.type)) == TYPE_CLASS &&
            types_.At(types_.Unqualified(field.type)).entity ==
                base_info.entity)
          offset = std::max(offset, base_entity.size);
      }
      field.offset = offset;
      offset += field_size;
    }
    size = std::max(size, value.is_union ? field_size : offset);
  }
  if (have_bit_unit)
    offset = std::max(offset, bit_unit_offset + bit_unit_size);
  bool empty = true;
  for (std::size_t i = 0; empty && i < value.bases.size(); ++i)
    empty = model_.ClassAt(value.bases[i].entity).empty;
  for (std::size_t i = 0; empty && i < value.fields.size(); ++i)
    empty = value.fields[i].static_member;
  value.empty = empty;

  // C++ gives every complete class object a nonzero size, and an object's
  // size is a multiple of its alignment.
  if (size == 0)
    size = 1;
  if (value.requested_alignment != 0 &&
      value.requested_alignment < alignment)
    throw std::runtime_error("class alignas weakens natural alignment");
  alignment = std::max(alignment, value.requested_alignment);
  size = AlignUp(size, alignment);
  value.size = size;
  value.alignment = alignment;
  value.layout_complete = true;
  value.aggregate = value.bases.empty();
  for (std::size_t i = 0; i < value.fields.size(); ++i) {
    const ClassField& field = value.fields[i];
    if (field.static_member)
      continue;
    if (field.access != ACCESS_PUBLIC || field.initializer != 0)
      value.aggregate = false;
  }
  for (std::size_t i = 0; i < value.constructors.size(); ++i) {
    const FunctionEntity& constructor =
        model_.FunctionAt(value.constructors[i]);
    if (!constructor.synthesized && !constructor.defaulted &&
        !constructor.deleted)
      value.aggregate = false;
  }
  value.trivial_default_constructor = true;
  for (std::size_t i = 0; i < value.constructors.size(); ++i)
    if (!model_.FunctionAt(value.constructors[i]).synthesized &&
        !model_.FunctionAt(value.constructors[i]).defaulted &&
        !model_.FunctionAt(value.constructors[i]).deleted)
      value.trivial_default_constructor = false;
  for (std::size_t i = 0; i < value.fields.size(); ++i) {
    if (value.fields[i].static_member)
      continue;
    if (value.fields[i].initializer != 0) {
      value.trivial_default_constructor = false;
      continue;
    }
    TypeId element = types_.Unqualified(value.fields[i].type);
    while (types_.Kind(element) == TYPE_ARRAY)
      element = types_.Unqualified(types_.At(element).base);
    if (types_.Kind(element) == TYPE_CLASS &&
        !model_.ClassAt(types_.At(element).entity)
            .trivial_default_constructor)
      value.trivial_default_constructor = false;
  }
  for (std::size_t i = 0; i < value.bases.size(); ++i)
    if (!model_.ClassAt(value.bases[i].entity).trivial_default_constructor)
      value.trivial_default_constructor = false;
  value.trivial_destructor = value.destructor == 0 ||
      model_.FunctionAt(value.destructor).synthesized;
  for (std::size_t i = 0; i < value.bases.size(); ++i)
    if (!model_.ClassAt(value.bases[i].entity).trivial_destructor)
      value.trivial_destructor = false;
  for (std::size_t i = 0; i < value.fields.size(); ++i)
  {
    if (value.fields[i].static_member)
      continue;
    TypeId element = types_.Unqualified(value.fields[i].type);
    while (types_.Kind(element) == TYPE_ARRAY)
      element = types_.Unqualified(types_.At(element).base);
    if (types_.Kind(element) == TYPE_CLASS &&
        !model_.ClassAt(types_.At(element).entity).trivial_destructor)
      value.trivial_destructor = false;
  }
  // 9.2/12.8: trivial copyability is a complete-object property.  A
  // user-provided copy or move member, a user-provided destructor, or a
  // non-trivially-copyable class subobject makes byte-wise transfer
  // invalid.  References are stored as addresses but are not class
  // subobjects, so a reference member does not clear the property.
  value.trivially_copyable = true;
  for (std::size_t i = 0; i < value.constructors.size(); ++i) {
    const FunctionEntity& constructor =
        model_.FunctionAt(value.constructors[i]);
    if ((constructor.copy_constructor || constructor.move_constructor) &&
        !constructor.defaulted)
      value.trivially_copyable = false;
  }
  if (value.destructor != 0 &&
      !model_.FunctionAt(value.destructor).synthesized)
    value.trivially_copyable = false;
  for (std::size_t i = 0; i < value.bases.size(); ++i)
    if (!model_.ClassAt(value.bases[i].entity).trivially_copyable)
      value.trivially_copyable = false;
  for (std::size_t i = 0; i < value.fields.size(); ++i) {
    if (value.fields[i].static_member)
      continue;
    TypeId element = types_.Unqualified(value.fields[i].type);
    while (types_.Kind(element) == TYPE_ARRAY)
      element = types_.Unqualified(types_.At(element).base);
    if (types_.Kind(element) == TYPE_CLASS &&
        !model_.ClassAt(types_.At(element).entity).trivially_copyable)
      value.trivially_copyable = false;
  }
  types_.SetClassLayout(entity, size, alignment);
}
