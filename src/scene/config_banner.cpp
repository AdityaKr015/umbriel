#include "scene/config_banner.h"

#include "config/config.h"
#include "config/config_diag.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <glib.h>
#include <string>
#include <wayland-server-core.h>

namespace {

  constexpr int kPadding = 14;
  constexpr int kMaxLines = 6;
  constexpr int kDefaultMaxWidth = 800;
  constexpr int kAbsMaxWidth = 960;
  constexpr int kTopMargin = 24;
  constexpr int kAutoHideMs = 10000;

  // Make a diagnostic file path short relative to the config root's directory.
  std::string shortPath(const umbriel::ConfigDiagnostic& diag, const std::filesystem::path& configDir) {
    if (diag.file.empty()) {
      return {};
    }
    std::string dirStr = configDir.string();
    if (!dirStr.empty() && dirStr.back() != '/') {
      dirStr += '/';
    }
    std::string fileStr = diag.file;
    if (fileStr.starts_with(dirStr)) {
      fileStr = fileStr.substr(dirStr.size());
    }
    std::string loc = fileStr;
    if (diag.line > 0) {
      loc += std::format(":{}", diag.line);
      if (diag.column > 0) {
        loc += std::format(":{}", diag.column);
      }
    }
    return loc;
  }

  int onHideTimer(void* data) {
    auto* banner = static_cast<umbriel::ConfigBanner*>(data);
    banner->hide();
    return 0; // disarm
  }

} // namespace

namespace umbriel {

  ConfigBanner::ConfigBanner(Server& server, wlr_scene_tree* parent)
      : m_server(server), m_parent(parent) {}

  ConfigBanner::~ConfigBanner() {
    hide();
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
    }
  }

  void ConfigBanner::show(const std::vector<ConfigDiagnostic>& diagnostics) {
    // Disarm any pending auto-hide.
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
      m_hideTimer = nullptr;
    }

    if (diagnostics.empty()) {
      hide();
      return;
    }

    m_lastDiagnostics = diagnostics;
    m_persistent = std::ranges::any_of(diagnostics, [](const ConfigDiagnostic& d) {
      return d.severity == ConfigDiagnostic::Severity::Error;
    });

    render(diagnostics);

    if (!m_persistent) {
      wl_event_loop* loop = wl_display_get_event_loop(m_server.display());
      m_hideTimer = wl_event_loop_add_timer(loop, onHideTimer, this);
      wl_event_source_timer_update(m_hideTimer, kAutoHideMs);
    }
  }

  void ConfigBanner::hide() {
    if (m_sceneBuffer != nullptr) {
      wlr_scene_node_destroy(&m_sceneBuffer->node);
      m_sceneBuffer = nullptr;
    }
    m_lastDiagnostics.clear();
    m_persistent = false;
    if (m_hideTimer != nullptr) {
      wl_event_source_remove(m_hideTimer);
      m_hideTimer = nullptr;
    }
  }

  void ConfigBanner::relayout() {
    if (m_lastDiagnostics.empty()) {
      return;
    }
    render(m_lastDiagnostics);
  }

  void ConfigBanner::render(const std::vector<ConfigDiagnostic>& diagnostics) {
    // Destroy previous buffer node.
    if (m_sceneBuffer != nullptr) {
      wlr_scene_node_destroy(&m_sceneBuffer->node);
      m_sceneBuffer = nullptr;
    }

    // Determine output scale and dimensions.
    wlr_output* output = m_server.preferredOutput();
    double scale = 1.0;
    int outputLogicalWidth = 0;
    wlr_box outputBox{};
    bool haveOutput = false;
    if (output != nullptr) {
      scale = std::max(1.0, std::ceil(static_cast<double>(output->scale)));
      wlr_output_layout_get_box(m_server.outputLayout(), output, &outputBox);
      outputLogicalWidth = outputBox.width;
      haveOutput = true;
    }

    int maxTextWidth = kDefaultMaxWidth;
    if (outputLogicalWidth > 0) {
      maxTextWidth = std::min(outputLogicalWidth - 80, kAbsMaxWidth);
      if (maxTextWidth < 200) {
        maxTextWidth = 200;
      }
    }

    // Determine heading.
    const bool hasError = std::ranges::any_of(diagnostics, [](const ConfigDiagnostic& diagnostic) {
      return diagnostic.severity == ConfigDiagnostic::Severity::Error;
    });
    const char* heading = hasError ? "Configuration errors" : "Configuration warnings";
    const char* headingColor = hasError ? "#ff6b6b" : "#f5c96b";

    // Build pango markup.
    const std::filesystem::path configDir = configRootPath().parent_path();
    std::string markup;
    {
      gchar* escapedHeading = g_markup_escape_text(heading, -1);
      markup += std::format("<span foreground='{}'>{}</span>", headingColor, escapedHeading);
      g_free(escapedHeading);
    }

    int shown = 0;
    const int total = static_cast<int>(diagnostics.size());
    for (int i = 0; i < total && shown < kMaxLines; ++i, ++shown) {
      const auto& diagnostic = diagnostics[static_cast<size_t>(i)];
      const std::string loc = shortPath(diagnostic, configDir);
      gchar* escapedMsg = g_markup_escape_text(diagnostic.message.c_str(), -1);
      if (loc.empty()) {
        markup += std::format("\n<span foreground='#e8e8ea'>{}</span>", escapedMsg);
      } else {
        gchar* escapedLoc = g_markup_escape_text(loc.c_str(), -1);
        markup += std::format("\n<span foreground='#e8e8ea'>{}: {}</span>", escapedLoc, escapedMsg);
        g_free(escapedLoc);
      }
      g_free(escapedMsg);
    }
    if (total > kMaxLines) {
      const int remaining = total - kMaxLines;
      markup += std::format("\n<span foreground='#e8e8ea'>+{} more — run `umbriel validate`</span>", remaining);
    }

    // Render text into a wlr_buffer via the shared utility.
    TextBufferResult rendered = renderTextBuffer({
        .markup = std::move(markup),
        .font = "monospace 11",
        .maxWidth = maxTextWidth,
        .padding = kPadding,
        .scale = scale,
    });
    if (rendered.buffer == nullptr) {
      return;
    }

    // Create scene buffer node.
    m_sceneBuffer = wlr_scene_buffer_create(m_parent, rendered.buffer);
    wlr_buffer_drop(rendered.buffer); // scene holds the lock
    if (m_sceneBuffer == nullptr) {
      return;
    }

    // Map device-pixel buffer to logical output size.
    wlr_scene_buffer_set_dest_size(m_sceneBuffer, rendered.logicalWidth, rendered.logicalHeight);

    // Input pass-through.
    m_sceneBuffer->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };

    // Position: top-center of preferred output.
    if (haveOutput) {
      const int x = outputBox.x + (outputBox.width - rendered.logicalWidth) / 2;
      const int y = outputBox.y + kTopMargin;
      wlr_scene_node_set_position(&m_sceneBuffer->node, x, y);
    } else {
      wlr_scene_node_set_position(&m_sceneBuffer->node, kTopMargin, kTopMargin);
    }
  }

} // namespace umbriel
