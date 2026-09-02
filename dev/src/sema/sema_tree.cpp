#include "sema/sema_tree.h"

#include <stdexcept>

SemaNode::SemaNode()
    : kind(SEMA_TRANSLATION_UNIT), category(VC_PRVALUE), type(0),
      op(KW_AUTO), binding(0), function(0), scope(0), first(0), last(0),
      has_value(false), value(0), first_child(0), last_child(0),
      next_sibling(0)
{
}

SemaTree::SemaTree()
    : nodes_(1), root_(0)
{
}

SemaId SemaTree::Make(SemaKind kind)
{
  SemaNode node;
  node.kind = kind;
  nodes_.push_back(node);
  return nodes_.size() - 1;
}

void SemaTree::Append(SemaId parent, SemaId child)
{
  if (parent == 0 || child == 0 || parent >= nodes_.size() ||
      child >= nodes_.size())
    throw std::out_of_range("invalid semantic tree link");
  SemaNode& owner = nodes_[parent];
  if (owner.first_child == 0)
    owner.first_child = child;
  else
    nodes_[owner.last_child].next_sibling = child;
  owner.last_child = child;
}

SemaNode& SemaTree::At(SemaId id)
{
  if (id == 0 || id >= nodes_.size())
    throw std::out_of_range("invalid semantic node");
  return nodes_[id];
}

const SemaNode& SemaTree::At(SemaId id) const
{
  if (id == 0 || id >= nodes_.size())
    throw std::out_of_range("invalid semantic node");
  return nodes_[id];
}

SemaId SemaTree::Root() const
{
  return root_;
}

void SemaTree::SetRoot(SemaId root)
{
  if (root == 0 || root >= nodes_.size())
    throw std::out_of_range("invalid semantic root");
  root_ = root;
}
