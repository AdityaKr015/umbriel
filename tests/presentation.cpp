#include "view/presentation.h"

#include "check.h"

// clang-format off
#include "wlr.h"
// clang-format on

UMBRIEL_TEST(fullscreenPlacementSurvivesSceneReconfiguration) {
  wlr_scene* scene = wlr_scene_create();
  CHECK(scene != nullptr);
  if (scene == nullptr) {
    return;
  }
  wlr_scene_tree* surfaceTree = wlr_scene_tree_create(&scene->tree);
  CHECK(surfaceTree != nullptr);
  if (surfaceTree == nullptr) {
    wlr_scene_node_destroy(&scene->tree.node);
    return;
  }

  umbriel::ViewPresentation presentation;
  const wlr_box geometry{0, 0, 1920, 1080};
  presentation.updateFullscreen(true, 2560, 1440, &surfaceTree->node, geometry);
  CHECK_EQ(surfaceTree->node.x, 320);
  CHECK_EQ(surfaceTree->node.y, 180);

  // A clip update reconfigures the scene surface and resets this position.
  wlr_scene_node_set_position(&surfaceTree->node, -geometry.x, -geometry.y);
  presentation.restoreFullscreenSurface(true, &surfaceTree->node, geometry);
  CHECK_EQ(surfaceTree->node.x, 320);
  CHECK_EQ(surfaceTree->node.y, 180);

  wlr_scene_node_destroy(&scene->tree.node);
}

int main() { return RUN_TESTS(); }
