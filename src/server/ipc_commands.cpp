#include "server/ipc_commands.h"

#include "config/config.h"
#include "layer/layer_surface.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <nlohmann/json.hpp>
#include <print>

namespace umbriel {

  namespace {
    const char* layerName(uint32_t layer) {
      switch (layer) {
      case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return "background";
      case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return "bottom";
      case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return "top";
      case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return "overlay";
      default:
        return "unknown";
      }
    }

    void printWindows(const nlohmann::json& ok) {
      for (const auto& entry : ok) {
        const std::string appId = entry.value("app_id", "");
        const std::string title = entry.value("title", "");
        std::println(
            "{}{}{}\t{}\t[{} {}x{}{:+}{:+}]",
            entry.value("focused", false) ? "*" : (entry.value("urgent", false) ? "!" : " "),
            entry.value("xwayland", false) ? "[Xwayland] " : "", appId.empty() ? "-" : appId,
            title.empty() ? "-" : title, entry.value("floating", false) ? "float" : "tile", entry.value("w", 0),
            entry.value("h", 0), entry.value("x", 0), entry.value("y", 0)
        );
      }
    }

    void printLayers(const nlohmann::json& ok) {
      for (const auto& entry : ok) {
        const std::string layer = entry.value("layer", "");
        const std::string ns = entry.value("namespace", "");
        const std::string output = entry.value("output", "");
        bool mapped = entry.value("mapped", false);
        std::println(
            "{}\t{}\t{}\t{}", layer, ns.empty() ? "-" : ns, output.empty() ? "-" : output, mapped ? "yes" : "no"
        );
      }
    }
  } // namespace

  nlohmann::json IpcCommands::windows(Server& server, std::string_view /*arg*/) {
    nlohmann::json windows = nlohmann::json::array();
    for (const auto& v : server.views()) {
      if (!v->mapped()) {
        continue;
      }
      nlohmann::json entry;
      entry["app_id"] = v->toplevel()->app_id != nullptr ? v->toplevel()->app_id : "";
      entry["title"] = v->toplevel()->title != nullptr ? v->toplevel()->title : "";
      entry["floating"] = v->floating();
      // The compositor's own notion of focus, which is what every action acts
      // on. Lets a caller (and the harness) see where focus went.
      entry["focused"] = v->workspace() != nullptr && v->workspace()->focusedView() == v.get();
      entry["urgent"] = v->urgent();
      entry["xwayland"] = v->xwayland();
      entry["x"] = v->sceneTree()->node.x;
      entry["y"] = v->sceneTree()->node.y;
      entry["w"] = v->toplevel()->base->geometry.width;
      entry["h"] = v->toplevel()->base->geometry.height;
      windows.push_back(std::move(entry));
    }
    return nlohmann::json{{"ok", windows}};
  }

  nlohmann::json IpcCommands::layers(Server& server, std::string_view /*arg*/) {
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& l : server.layerSurfaces()) {
      auto* s = l->layerSurface();
      nlohmann::json entry;
      entry["layer"] = layerName(s->current.layer);
      entry["namespace"] = s->namespace_ != nullptr ? s->namespace_ : "";
      entry["output"] = s->output != nullptr ? s->output->name : "";
      entry["mapped"] = l->mapped();
      layers.push_back(std::move(entry));
    }
    return nlohmann::json{{"ok", layers}};
  }

  nlohmann::json IpcCommands::msg(Server& server, std::string_view arg) {
    Keybind bind{};
    if (!parseAction(std::string(arg), bind)) {
      return nlohmann::json{{"err", "unknown action: " + std::string(arg)}};
    }
    std::string error;
    server.executeKeybindAction(bind, &error);
    if (!error.empty()) {
      return nlohmann::json{{"err", error}};
    }
    return nlohmann::json{{"ok", nullptr}};
  }

  static constexpr IpcCommandSpec kIpcCommands[] = {
      {"layers", "", "list layer-shell surfaces", false, &IpcCommands::layers, &printLayers},
      {"msg", "<action> [args...]", "send an action to the compositor", true, &IpcCommands::msg, nullptr},
      {"windows", "", "list windows (app id and title)", false, &IpcCommands::windows, &printWindows},
  };

  std::span<const IpcCommandSpec> ipcCommands() { return kIpcCommands; }

  const IpcCommandSpec* findIpcCommand(std::string_view name) {
    for (const auto& spec : kIpcCommands) {
      if (name == spec.name) {
        return &spec;
      }
    }
    return nullptr;
  }

} // namespace umbriel
