#pragma once

#include <string>
#include <vector>

struct wlr_scene_tree;

namespace umbriel {

  class Output;
  class Server;
  class View;
  class Workspace;
  class ScratchpadManager {
  public:
    ScratchpadManager(Server& server, wlr_scene_tree* root, wlr_scene_tree* shadowRoot);

    [[nodiscard]] bool contains(const View* view) const;
    [[nodiscard]] bool moveToScratchpad(View* view, Output* output);
    bool toggle(Output* output);
    bool restoreFocused(Output* output);
    bool focusNext(Output* output);
    [[nodiscard]] View* focused(Output* output) const;
    [[nodiscard]] bool hasFocus(Output* output) const;
    void noteFocus(View* view);
    void finishMove(View* view, Output* output);
    // Restore the manager-owned scene parents after a temporary global drag.
    void restorePresentation(View* view);
    void remove(View* view);
    void moveOutput(Output* from, Output* to);
    void adoptOrphans(Output* output);

  private:
    struct Entry {
      View* view = nullptr;
      Output* output = nullptr;
      std::string returnOutput;
      std::string returnWorkspace;
      bool returnTiled = false;
      bool lastFocused = false;
    };

    void setVisible(Output* output, bool visible);

    Server* m_server = nullptr;
    wlr_scene_tree* m_root = nullptr;
    wlr_scene_tree* m_shadowRoot = nullptr;
    std::vector<Entry> m_entries;
    std::vector<Output*> m_visibleOutputs;
    View* m_focusedView = nullptr;
  };

} // namespace umbriel
