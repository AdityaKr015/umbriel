#pragma once

namespace umbriel {

enum class SceneNodeKind {
  View,
  LayerSurface,
  LockSurface,
};

// Stored in wlr_scene_node::data so hit-testing can tell views from layers
// (including xdg popups parented to a layer surface).
struct SceneNode {
  explicit SceneNode(SceneNodeKind kind) : kind(kind) {}

  SceneNodeKind kind;
};

} // namespace umbriel
