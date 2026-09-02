#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "parser/ast_model.h"
#include "sema/qualified_name.h"
#include "sema/type_table.h"

// The scope tree: scopes own bindings in declaration order and child scopes
// in creation order (both are dump order); a per-scope name index serves
// lookup once a scope grows past a handful of names.  Class, enum and
// function entities carry the facts shared by all declarations of one
// entity.  Every scope reachable through a binding is derived on demand from
// the binding's kind and type, so no binding holds a copy of a scope that may
// not exist yet.
typedef std::size_t ScopeId;
typedef std::size_t BindingId;
typedef std::size_t ClassEntityId;
typedef std::size_t EnumEntityId;
typedef std::size_t FunctionEntityId;

enum ScopeKind
{
  SCOPE_NAMESPACE,
  SCOPE_CLASS,
  SCOPE_FUNCTION,
  SCOPE_BLOCK,
  SCOPE_ENUM,
  SCOPE_TEMPLATE_PARAMETERS
};

enum BindingKind
{
  BINDING_TYPE,
  BINDING_TYPE_ALIAS,
  BINDING_VARIABLE,
  BINDING_FUNCTION,
  BINDING_NAMESPACE,
  BINDING_ENUMERATOR,
  BINDING_PARAMETER
};

// Access and layout facts belong to the class entity, rather than to the
// parser's declaration nodes.  Keeping these as small enums/records also
// lets lookup and lowering share one canonical member identity.
enum AccessKind
{
  ACCESS_PUBLIC,
  ACCESS_PROTECTED,
  ACCESS_PRIVATE
};

enum SpecialMemberKind
{
  SPECIAL_MEMBER_NONE,
  SPECIAL_MEMBER_CONSTRUCTOR,
  SPECIAL_MEMBER_DESTRUCTOR
};

enum LookupFilter
{
  LOOKUP_TYPES = 1u << 0,
  LOOKUP_VALUES = 1u << 1,
  LOOKUP_FUNCTIONS = 1u << 2,
  LOOKUP_NAMESPACES = 1u << 3,
  LOOKUP_ENUMERATORS = 1u << 4,
  // A friend defined in a class is declared in the innermost enclosing
  // namespace, but remains invisible to ordinary namespace lookup until a
  // matching namespace-scope declaration is provided.  ADL opts into this
  // bit while collecting the hidden friends associated with an argument.
  LOOKUP_HIDDEN_FRIENDS = 1u << 5,
  LOOKUP_ANY = LOOKUP_TYPES | LOOKUP_VALUES | LOOKUP_FUNCTIONS |
      LOOKUP_NAMESPACES | LOOKUP_ENUMERATORS,
  // 3.4.3p1: the name before `::` ignores objects, functions and enumerators.
  LOOKUP_QUALIFIER = LOOKUP_TYPES | LOOKUP_NAMESPACES
};

struct Binding
{
  // Printed spelling.  A qualified out-of-class enum definition keeps its
  // qualified spelling as the dump requires; lookup never reads that form.
  std::string name;
  BindingKind kind;
  TypeId type;
  ScopeId scope; // owning declaration scope
  ScopeId namespace_scope; // BINDING_NAMESPACE: the nominated namespace
  FunctionEntityId function; // BINDING_FUNCTION: canonical function entity
  BindingId object_binding; // injected member: implicit anonymous-union object
  bool internal_linkage;
  bool c_linkage;
  bool extern_declaration;
  bool hidden_friend;
  bool noexcept_qualifier;
  AccessKind access;
  bool static_member;
  bool bit_field;
  unsigned bit_width;
  // BINDING_VARIABLE at namespace scope: the first binding of the same
  // object when this declaration redeclares it (3.3.10); 0 when this is the
  // first declaration.  Consumers that need one symbol per object key on it.
  BindingId redeclared_binding;
  bool has_const_value;
  long long const_value;

  Binding();
};

struct ClassBase
{
  ClassEntityId entity;
  AccessKind access;
  std::size_t offset;

  ClassBase(ClassEntityId entity = 0, AccessKind access = ACCESS_PUBLIC,
            std::size_t offset = 0)
      : entity(entity), access(access), offset(offset) {}
};

struct ClassField
{
  BindingId binding;
  TypeId type;
  std::size_t offset;
  std::size_t bit_offset;
  unsigned bit_width;
  AccessKind access;
  AstId initializer;
  bool static_member;

  ClassField(BindingId binding = 0, TypeId type = 0)
      : binding(binding), type(type), offset(0), bit_offset(0),
        bit_width(0), access(ACCESS_PUBLIC), initializer(0),
        static_member(false) {}
};

struct Scope
{
  ScopeKind kind;
  std::string name; // printed after the scope kind; empty for blocks and template scopes
  ScopeId parent;
  ClassEntityId class_entity; // SCOPE_CLASS: the class whose members it holds
  bool inline_namespace;
  bool unnamed_namespace;
  std::vector<BindingId> bindings;
  std::vector<ScopeId> children;
  std::vector<ScopeId> inline_namespaces; // 7.3.1p8: searched as members
  // 7.3.4p2: a directive's names appear as if declared in the nearest
  // enclosing namespace that contains both the directive and the nominated
  // namespace; that namespace is recorded once when the directive is added.
  struct UsingDirective
  {
    ScopeId nominated;
    ScopeId apply_at;

    UsingDirective(ScopeId nominated = 0, ScopeId apply_at = 0)
        : nominated(nominated), apply_at(apply_at) {}
  };
  std::vector<UsingDirective> using_directives;
  // name -> bindings of that name in declaration order; built once the scope
  // holds more than kSmallScope bindings, empty before that.
  std::unordered_map<std::string, std::vector<BindingId> > index;

  Scope();
};

struct ClassEntity
{
  ScopeId class_scope; // 0 until defined
  TypeId type; // canonical class type, 0 until declared
  FunctionEntityId default_constructor; // synthesized on first default-init
  std::vector<FunctionEntityId> constructors; // overload set, declaration order
  // 9.5p5: members injected into the enclosing scope, each naming the
  // synthesized object through Binding::object_binding.
  std::vector<BindingId> injected_members;
  std::vector<ClassBase> bases;
  std::vector<ClassField> fields;
  std::size_t size;
  std::size_t alignment;
  std::size_t requested_alignment;
  FunctionEntityId constructor;
  FunctionEntityId destructor;
  ClassEntityId inheriting_constructor_base;
  // Friend declarations are owned by their innermost enclosing class for
  // ADL, even though their bindings live in that class's enclosing
  // namespace.  This is the canonical hidden-friend association set.
  std::vector<BindingId> hidden_friends;
  bool layout_complete;
  bool trivial_default_constructor;
  bool aggregate;
  bool is_union;
  bool defined;

  ClassEntity();
};

struct EnumEntity
{
  // Scope holding the enumerators: the enum scope of a scoped enumeration or
  // the declaring scope of a defined unscoped one; 0 before that exists.
  ScopeId enum_scope;
  TypeId underlying;
  bool scoped;
  bool defined;

  EnumEntity();
};

struct FunctionEntity
{
  ScopeId scope; // declaration scope, not the function body's block scope
  std::string name;
  TypeId type; // canonical function type with adjusted parameters
  TypeId member_type; // member signature without the implicit this parameter
  TypeId member_pointer_type; // address-of type for a member function
  ClassEntityId member_class;
  bool is_member;
  bool member_const;
  bool member_volatile;
  bool is_template;
  std::vector<TypeId> template_parameters; // TYPE_TEMPLATE_PARAM types, in order
  bool internal_linkage;
  bool c_linkage;
  bool noexcept_qualifier;
  SpecialMemberKind special_member;
  AstId body;
  AstId ctor_initializer;
  std::vector<std::string> parameter_names;
  bool defaulted;
  bool deleted;
  bool static_member;
  bool in_class_definition;
  bool synthesized;
  // One entry per canonical parameter; a zero entry means that parameter
  // has no default initializer.  The AST initializer remains the canonical
  // source fact and is materialized at each call site by expression sema.
  std::vector<AstId> default_arguments;
  // Lowering needs the already analyzed form when an implicitly initialized
  // base or member consumes a constructor default.  These nodes are kept out
  // of the source semantic tree but retain the declaration-time lookup scope.
  std::vector<std::size_t> default_semantic_arguments;
  std::vector<std::pair<BindingId, std::size_t> >
      default_member_initializers;
  bool defined;

  FunctionEntity();
};

class SemaModel
{
public:
  explicit SemaModel(TypeTable& types);

  ScopeId GlobalScope() const;
  ScopeId CreateScope(ScopeKind kind, const std::string& name,
                      ScopeId parent, bool inline_namespace = false);
  // A reopened namespace may become inline (7.3.1p2).
  void MarkInlineNamespace(ScopeId scope);
  Scope& ScopeAt(ScopeId id);
  const Scope& ScopeAt(ScopeId id) const;

  BindingId AddBinding(ScopeId scope, const std::string& name,
                       BindingKind kind, TypeId type = 0,
                       ScopeId namespace_scope = 0);
  void AddUsingDirective(ScopeId scope, ScopeId target);
  Binding& BindingAt(BindingId id);
  const Binding& BindingAt(BindingId id) const;

  // Latest binding of `name` declared directly in `scope` that passes the
  // filter; 0 when none.
  BindingId DirectBinding(ScopeId scope, const std::string& name,
                          unsigned filter = LOOKUP_ANY) const;
  // Every binding of `name` declared directly in `scope` that passes the
  // filter, in declaration order (appended to `result`).
  void DirectBindings(ScopeId scope, const std::string& name,
                      unsigned filter, std::vector<BindingId>& result) const;
  // 3.4.1 unqualified lookup restricted to the filter (elaborated lookup
  // uses LOOKUP_TYPES, the qualifier position LOOKUP_QUALIFIER): the latest
  // declaration at the nearest level that declares the name.
  BindingId LookupUnqualified(ScopeId scope, const std::string& name,
                              unsigned filter) const;
  // Overload-aware forms.  The result is the complete set from the first
  // lookup level that contains a match; a non-function in that set is kept so
  // callers can enforce ordinary-name hiding before attempting a call.
  void LookupSet(ScopeId scope, const std::string& name, unsigned filter,
                 std::vector<BindingId>& result) const;
  void LookupQualifiedSet(ScopeId scope, const QualifiedName& name,
                          unsigned filter,
                          std::vector<BindingId>& result) const;
  // Ordinary lookup of a type name: a later object, function, enumerator or
  // parameter of the same name hides the type (3.3.10p2).
  BindingId LookupTypeName(ScopeId scope, const std::string& name) const;
  // 3.4.3 qualified lookup through namespaces, class scopes and enums.
  BindingId LookupQualified(ScopeId scope, const QualifiedName& name,
                            unsigned filter) const;
  // Dispatches on the name form: global, qualified, or unqualified.
  BindingId Lookup(ScopeId scope, const QualifiedName& name,
                   unsigned filter) const;
  // Scope a binding denotes when used as a qualifier: the nominated
  // namespace, the member scope of a class, or the enumerator scope of an
  // enumeration (also through a type alias).  False when it has none yet.
  bool NominatedScope(BindingId binding, ScopeId& scope) const;
  // Member scope of a class type or enumerator scope of an enum type.
  bool ScopeOfType(TypeId type, ScopeId& scope) const;
  // Inheritance is a semantic relation owned by the class graph.  Consumers
  // such as overload ranking and LowIR use this query instead of comparing
  // class type spellings.
  bool IsDerivedFrom(ClassEntityId derived, ClassEntityId base) const;
  // Class member lookup applies ordinary hiding at each class level, then
  // searches bases.  The result may contain an overload set or an ambiguity.
  void LookupMember(ClassEntityId entity, const std::string& name,
                    unsigned filter, std::vector<BindingId>& result) const;
  // 13.3.1.2: ordinary operator lookup plus the associated namespaces and
  // hidden friends of the operand types.  The caller adds member candidates
  // for the left operand separately because their implicit object has a
  // different conversion sequence from a non-member argument.
  void LookupOperatorSet(ScopeId scope, const std::string& name,
                         const std::vector<TypeId>& argument_types,
                         std::vector<BindingId>& result) const;
  // Unqualified function-call lookup uses the same ordinary-plus-ADL
  // candidate set, but is kept separate at the API boundary so expression
  // lookup does not accidentally apply ADL to a bare value expression.
  void LookupCallSet(ScopeId scope, const std::string& name,
                     const std::vector<TypeId>& argument_types,
                     std::vector<BindingId>& result) const;

  ClassEntityId CreateClass(bool is_union);
  ClassEntity& ClassAt(ClassEntityId id);
  const ClassEntity& ClassAt(ClassEntityId id) const;
  // The class whose member scope `scope` is, looking through the
  // template-parameter scopes of member templates; false for other scopes.
  bool ClassForScope(ScopeId scope, ClassEntityId& entity) const;
  EnumEntityId CreateEnum(bool scoped, TypeId underlying);
  EnumEntity& EnumAt(EnumEntityId id);
  const EnumEntity& EnumAt(EnumEntityId id) const;
  FunctionEntityId CreateFunction(ScopeId scope, const std::string& name,
                                  TypeId type);
  FunctionEntity& FunctionAt(FunctionEntityId id);
  const FunctionEntity& FunctionAt(FunctionEntityId id) const;
  TypeTable& Types();
  const TypeTable& Types() const;

private:
  static const std::size_t kSmallScope = 8;

  static bool Matches(const Binding& binding, unsigned filter);
  // Single-result choice from one lookup level: its latest own declaration,
  // else the first name the level sees through inline namespaces or
  // using-directives.
  BindingId Latest(const std::vector<BindingId>& found, ScopeId level) const;
  // 3.4.3.2p2 qualified namespace search: the namespace's own names, then
  // its inline namespaces, then the namespaces it nominates; each namespace
  // is searched once per lookup.
  void CollectNamespace(ScopeId scope, const std::string& name,
                        unsigned filter, std::vector<ScopeId>& visited,
                        std::vector<BindingId>& result) const;
  // 3.4.1 outward walk; returns the level that declared the name or
  // kNoScope.  With `hide_types`, an object, function, enumerator or
  // parameter declared at a level hides any type of that name (3.3.10p2).
  ScopeId WalkUnqualified(ScopeId scope, const std::string& name,
                          unsigned filter, bool hide_types,
                          std::vector<BindingId>& result) const;
  // 3.4.3: the scope named by every component before the last, plus the
  // unscoped enumeration when that component names one.
  bool ResolveQualifier(ScopeId scope, const QualifiedName& name,
                        ScopeId& target, EntityId& unscoped_enum) const;
  bool StepInto(BindingId prefix, ScopeId& target,
                EntityId& unscoped_enum) const;
  BindingId SearchMember(ScopeId scope, EntityId unscoped_enum,
                         const std::string& name, unsigned filter) const;
  void CollectMember(ScopeId scope, EntityId unscoped_enum,
                     const std::string& name, unsigned filter,
                     std::vector<BindingId>& result) const;
  void CollectClassMember(ClassEntityId entity, const std::string& name,
                          unsigned filter, std::vector<ClassEntityId>& visited,
                          std::vector<BindingId>& result) const;
  void CollectAssociated(TypeId type, std::vector<ScopeId>& namespaces,
                         std::vector<ClassEntityId>& classes,
                         std::vector<TypeId>& visited_types) const;
  void CollectAssociatedClass(ClassEntityId entity,
                              std::vector<ScopeId>& namespaces,
                              std::vector<ClassEntityId>& classes) const;
  void AddAssociatedNamespace(ScopeId scope,
                              std::vector<ScopeId>& namespaces) const;
  void CollectAssociatedNamespace(
      ScopeId scope, const std::string& name, unsigned filter,
      std::vector<ScopeId>& visited,
      std::vector<BindingId>& result) const;

  static const ScopeId kNoScope = static_cast<ScopeId>(-1);

  TypeTable& types_;
  std::vector<Scope> scopes_;
  std::vector<Binding> bindings_;
  std::vector<ClassEntity> classes_;
  std::vector<EnumEntity> enums_;
  std::vector<FunctionEntity> functions_;
};
