#include "fg/FrameGraph.hpp"
#include "fg/GraphvizWriter.hpp"
#include <stack>

//
// FrameGraph class:
//

void FrameGraph::reserve(uint32_t numPasses, uint32_t numResources) {
  m_passNodes.reserve(numPasses);
  m_resourceNodes.reserve(numResources);
  m_resourceRegistry.reserve(numResources);
}

bool FrameGraph::isValid(FrameGraphResource id) const {
  const auto &node = _getResourceNode(id);
  auto &resource = m_resourceRegistry[node.m_resourceId];
  return node.m_version == resource.m_version;
}

void FrameGraph::compile() {
  for (auto &pass : m_passNodes) {
    pass.m_refCount = pass.m_writes.size();
    for (auto &[id, _] : pass.m_reads) {
      auto &consumed = m_resourceNodes[id];
      consumed.m_refCount++;
    }
    for (auto &[id, _] : pass.m_writes) {
      auto &written = m_resourceNodes[id];
      written.m_producer = &pass;
    }
  }

  // -- Culling:

  std::stack<ResourceNode *> unreferencedResources;
  for (auto &node : m_resourceNodes)
    if (node.m_refCount == 0) unreferencedResources.push(&node);

  while (!unreferencedResources.empty()) {
    auto *unreferencedResource = unreferencedResources.top();
    unreferencedResources.pop();
    PassNode *producer{unreferencedResource->m_producer};
    if (producer == nullptr || producer->hasSideEffect()) continue;

    assert(producer->m_refCount >= 1);
    if (--producer->m_refCount == 0) {
      for (auto &[id, _] : producer->m_reads) {
        auto &node = m_resourceNodes[id];
        if (--node.m_refCount == 0) unreferencedResources.push(&node);
      }
    }
  }

  // -- Calculate resources lifetime:

  for (auto &pass : m_passNodes) {
    if (pass.m_refCount == 0) continue;

    for (auto id : pass.m_creates)
      _getResourceEntry(id).m_producer = &pass;
    for (auto &[id, _] : pass.m_writes)
      _getResourceEntry(id).m_last = &pass;
    for (auto &[id, _] : pass.m_reads)
      _getResourceEntry(id).m_last = &pass;
  }
}
void FrameGraph::execute(void *context, void *allocator) {
  for (auto &pass : m_passNodes) {
    if (!pass.canExecute()) continue;

    for (auto id : pass.m_creates)
      _getResourceEntry(id).create(allocator);

    for (auto &&[id, flags] : pass.m_reads) {
      if (flags != kFlagsIgnored) _getResourceEntry(id).preRead(flags, context);
    }
    for (auto &&[id, flags] : pass.m_writes) {
      if (flags != kFlagsIgnored)
        _getResourceEntry(id).preWrite(flags, context);
    }
    FrameGraphPassResources resources{*this, pass};
    std::invoke(*pass.m_exec, resources, context);

    for (auto &entry : m_resourceRegistry)
      if (entry.m_last == &pass && entry.isTransient())
        entry.destroy(allocator);
  }
}

// ---

PassNode &
FrameGraph::_createPassNode(const std::string_view name,
                            std::unique_ptr<FrameGraphPassConcept> &&base) {
  const auto id = static_cast<uint32_t>(m_passNodes.size());
  return m_passNodes.emplace_back(PassNode{name, id, std::move(base)});
}

ResourceNode &FrameGraph::_createResourceNode(const std::string_view name,
                                              uint32_t resourceId) {
  const auto id = static_cast<uint32_t>(m_resourceNodes.size());
  return m_resourceNodes.emplace_back(
    ResourceNode{name, id, resourceId, kResourceInitialVersion});
}
FrameGraphResource FrameGraph::_clone(FrameGraphResource id) {
  const auto &node = _getResourceNode(id);
  assert(node.m_resourceId < m_resourceRegistry.size());
  auto &entry = m_resourceRegistry[node.m_resourceId];
  entry.m_version++;

  const auto cloneId = static_cast<uint32_t>(m_resourceNodes.size());
  m_resourceNodes.emplace_back(ResourceNode{
    node.getName(),
    cloneId,
    node.m_resourceId,
    entry.getVersion(),
  });
  return cloneId;
}

const ResourceNode &FrameGraph::_getResourceNode(FrameGraphResource id) const {
  assert(id < m_resourceNodes.size());
  return m_resourceNodes[id];
}
ResourceEntry &FrameGraph::_getResourceEntry(FrameGraphResource id) {
  const auto &node = _getResourceNode(id);
  assert(node.m_resourceId < m_resourceRegistry.size());
  return m_resourceRegistry[node.m_resourceId];
}

// ---

std::ostream &operator<<(std::ostream &os, const FrameGraph &fg) {
  return fg.debugOutput(os, graphviz::Writer{});
}

//
// FrameGraph::Builder class:
//

FrameGraphResource FrameGraph::Builder::read(FrameGraphResource id,
                                             uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  return m_passNode._read(id, flags);
}
FrameGraphResource FrameGraph::Builder::write(FrameGraphResource id,
                                              uint32_t flags) {
  assert(m_frameGraph.isValid(id));
  if (m_frameGraph._getResourceEntry(id).isImported()) setSideEffect();

  if (m_passNode.creates(id)) {
    return m_passNode._write(id, flags);
  } else {
    // Writing to a texture produces a renamed handle.
    // This allows us to catch errors when resources are modified in
    // undefined order (when same resource is written by different passes).
    // Renaming resources enforces a specific execution order of the render
    // passes.
    m_passNode._read(id, kFlagsIgnored);
    return m_passNode._write(m_frameGraph._clone(id), flags);
  }
}

FrameGraph::Builder &FrameGraph::Builder::setSideEffect() {
  m_passNode.m_hasSideEffect = true;
  return *this;
}

FrameGraph::Builder::Builder(FrameGraph &fg, PassNode &node)
    : m_frameGraph{fg}, m_passNode{node} {}

//
// FrameGraphPassResources class:
//

FrameGraphPassResources::FrameGraphPassResources(FrameGraph &fg, PassNode &node)
    : m_frameGraph{fg}, m_passNode{node} {}
