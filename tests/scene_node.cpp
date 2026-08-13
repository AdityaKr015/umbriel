#include "check.h"
#include "scene/node.h"

#include <cstdint>

using umbriel::SceneNode;
using umbriel::sceneNodeFrom;
using umbriel::SceneNodeKind;

namespace {

  // Stands in for View / LayerSurface / LockSurface, which all derive from
  // SceneNode as their single base and store `this` in wlr_scene_node::data.
  struct TaggedNode : SceneNode {
    explicit TaggedNode(SceneNodeKind kind) : SceneNode(kind) {}
    int payload = 42;
  };

  // Stands in for user data some other library stashed in the same field.
  struct ForeignData {
    uint32_t somethingElse = 0xDEADBEEF;
    void* pointer = nullptr;
  };

} // namespace

UMBRIEL_TEST(nullDataYieldsNull) { CHECK(sceneNodeFrom(nullptr) == nullptr); }

UMBRIEL_TEST(recoversATaggedNode) {
  SceneNode node(SceneNodeKind::View);
  SceneNode* recovered = sceneNodeFrom(&node);
  CHECK(recovered == &node);
  CHECK(recovered != nullptr && recovered->kind == SceneNodeKind::View);
}

UMBRIEL_TEST(recoversThroughADerivedPointer) {
  // The real call sites store a derived `this` and read it back as SceneNode*.
  TaggedNode layer(SceneNodeKind::LayerSurface);
  void* stored = &layer;

  SceneNode* recovered = sceneNodeFrom(stored);
  CHECK(recovered != nullptr);
  CHECK(recovered != nullptr && recovered->kind == SceneNodeKind::LayerSurface);
  CHECK(static_cast<TaggedNode*>(recovered) == &layer);
  CHECK(static_cast<TaggedNode*>(recovered)->payload == 42);
}

UMBRIEL_TEST(everyKindRoundTrips) {
  for (SceneNodeKind kind : {SceneNodeKind::View, SceneNodeKind::LayerSurface, SceneNodeKind::LockSurface}) {
    TaggedNode node(kind);
    SceneNode* recovered = sceneNodeFrom(&node);
    CHECK(recovered != nullptr);
    CHECK(recovered != nullptr && recovered->kind == kind);
  }
}

UMBRIEL_TEST(foreignDataYieldsNullInsteadOfBeingReinterpreted) {
  // This is the case that used to be silent undefined behavior: the pointer was
  // cast to SceneNode* and its `kind` read regardless of what it pointed at.
  ForeignData foreign;
  CHECK(sceneNodeFrom(&foreign) == nullptr);
}

UMBRIEL_TEST(magicIsTheFirstMemberSoTheGuardReadsIt) {
  // sceneNodeFrom reads `magic` through a possibly-foreign pointer, so it must
  // sit at offset 0 for the check to be meaningful.
  SceneNode node(SceneNodeKind::View);
  CHECK_EQ(reinterpret_cast<const char*>(&node.magic), reinterpret_cast<const char*>(&node));
  CHECK_EQ(node.magic, SceneNode::kMagic);
}

int main() { return RUN_TESTS(); }
