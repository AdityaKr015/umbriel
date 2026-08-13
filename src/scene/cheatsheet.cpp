#include "scene/cheatsheet.h"

#include "config/config.h"
#include "scene/cheatsheet_rows.h"
#include "scene/text_buffer.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

  constexpr int kPad = 28;
  constexpr int kCornerRadius = 16;
  constexpr int kColumnGap = 32;
  constexpr int kTitleBodyGap = 12;
  constexpr int kBodyFooterGap = 12;
  constexpr int kColumnMaxWidth = 600;
  constexpr int kMaxColumns = 4;
  constexpr float kPanelColor[] = {0.078F, 0.078F, 0.098F, 0.94F};

  // --- Chord reconstruction helpers ---

  // Escape text for Pango markup.
  std::string escape(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result(escaped);
    g_free(escaped);
    return result;
  }

} // namespace

namespace umbriel {

  Cheatsheet::Cheatsheet(Server& server, wlr_scene_tree* parent) : m_server(server), m_parent(parent) {}

  Cheatsheet::~Cheatsheet() { hide(); }

  void Cheatsheet::show() {
    if (m_server.sessionLocked()) {
      return;
    }
    render();
  }

  void Cheatsheet::hide() {
    if (m_tree == nullptr) {
      return;
    }
    m_shadow.reset();
    wlr_scene_node_destroy(&m_tree->node);
    m_tree = nullptr;
  }

  void Cheatsheet::toggle() {
    if (m_tree != nullptr) {
      hide();
    } else {
      show();
    }
  }

  bool Cheatsheet::visible() const { return m_tree != nullptr; }

  void Cheatsheet::relayout() {
    if (m_tree != nullptr) {
      render();
    }
  }

  void Cheatsheet::render() {
    // Destroy previous subtree.
    if (m_tree != nullptr) {
      m_shadow.reset();
      wlr_scene_node_destroy(&m_tree->node);
      m_tree = nullptr;
    }

    m_tree = wlr_scene_tree_create(m_parent);

    // --- Output info ---
    wlr_output* output = m_server.preferredOutput();
    double scale = 1.0;
    wlr_box outputBox{};
    bool haveOutput = false;
    if (output != nullptr) {
      scale = std::max(1.0, std::ceil(static_cast<double>(output->scale)));
      wlr_output_layout_get_box(m_server.outputLayout(), output, &outputBox);
      haveOutput = true;
    }

    std::vector<CheatsheetRow> rows = buildCheatsheetRows(config().keybinds);

    // --- Step 2: Group rows ---
    // Fixed groups in display order, then submaps in first-seen order.
    constexpr Group kFixedGroups[] = {
        Group::Apps, Group::Focus, Group::MoveSize, Group::Windows, Group::Workspaces, Group::Overview, Group::System,
    };

    // Collect submaps in first-seen order.
    std::vector<std::string> submapOrder;
    for (const auto& row : rows) {
      if (!row.submap.empty()) {
        if (std::ranges::find(submapOrder, row.submap) == submapOrder.end()) {
          submapOrder.push_back(row.submap);
        }
      }
    }

    // A display line is either a group header or a bind row.
    struct DisplayLine {
      bool isHeader = false;
      bool isDitto = false;
      int group = 0;    // group index: lines with the same value are never split across columns
      std::string text; // Pango markup for the full line (header or row)
    };

    auto buildLines = [&]() -> std::vector<DisplayLine> {
      std::vector<DisplayLine> lines;
      int groupId = 0;

      for (Group grp : kFixedGroups) {
        std::vector<const CheatsheetRow*> groupRows;
        for (const auto& row : rows) {
          if (!row.submap.empty())
            continue;
          if (groupForAction(row.actionType) == grp) {
            groupRows.push_back(&row);
          }
        }
        if (groupRows.empty())
          continue;

        const char* title = groupTitle(grp);
        // Blank separator belongs to the upcoming group (break happens before it).
        ++groupId;
        if (!lines.empty()) {
          lines.push_back({.isHeader = false, .group = groupId, .text = ""});
        }
        lines.push_back({
            .isHeader = true,
            .group = groupId,
            .text = std::format("<span foreground='#f5c96b' weight='bold'>{}</span>", title),
        });

        // Find max chord width in this group (character count, for padding).
        size_t maxChordLen = 0;
        for (const auto* row : groupRows) {
          maxChordLen = std::max(maxChordLen, row->chord.size());
        }

        // Apps group: single-usage binaries stay here as flat rows.
        // Multi-usage binaries are deferred to their own top-level groups
        // (rendered right after Apps with the same header style).
        if (grp == Group::Apps) {
          // Collect unique binaries in first-seen order.
          std::vector<std::string> binOrder;
          for (const auto* row : groupRows) {
            const std::string& bin = row->spawnBinary;
            if (std::ranges::find(binOrder, bin) == binOrder.end()) {
              binOrder.push_back(bin);
            }
          }

          // Partition: single-usage binaries render flat under "Apps",
          // multi-usage binaries are collected for their own groups.
          struct DeferredGroup {
            std::string title;
            std::vector<const CheatsheetRow*> rows;
          };
          std::vector<DeferredGroup> deferred;

          for (const auto& bin : binOrder) {
            std::vector<const CheatsheetRow*> binRows;
            for (const auto* row : groupRows) {
              if (row->spawnBinary == bin) {
                binRows.push_back(row);
              }
            }

            const auto realCount =
                std::ranges::count_if(binRows, [](const CheatsheetRow* r) { return r->action != "\xe2\x80\xb3"; });
            const bool promote = realCount >= 2
                && std::ranges::any_of(binRows, [](const CheatsheetRow* r) { return !r->spawnArgs.empty(); });

            if (promote) {
              // Capitalize first letter for the group title.
              std::string groupTitle = bin;
              if (!groupTitle.empty()) {
                groupTitle[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(groupTitle[0])));
              }
              deferred.push_back({.title = std::move(groupTitle), .rows = std::move(binRows)});
            } else {
              // Flat under "Apps": combine binary + args.
              for (const auto* row : binRows) {
                std::string escapedChord = escape(row->chord);
                const bool isDitto = row->action == "\xe2\x80\xb3";
                std::string label;
                if (isDitto) {
                  label = row->action;
                } else {
                  label = row->spawnArgs.empty() ? row->spawnBinary : row->spawnBinary + " " + row->action;
                  if (label.size() > 32) {
                    label = label.substr(0, 32) + "\xe2\x80\xa6";
                  }
                }
                std::string escapedAction = escape(label);
                size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
                std::string padding(extraPad, ' ');
                const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
                lines.push_back({
                    .isHeader = false,
                    .isDitto = isDitto,
                    .group = groupId,
                    .text = std::format(
                        "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span "
                        "foreground='{}'>{}</span>",
                        escapedChord, padding, actionColor, escapedAction
                    ),
                });
              }
            }
          }

          // Render deferred binary groups as top-level sections.
          for (auto& dg : deferred) {
            ++groupId;
            if (!lines.empty()) {
              lines.push_back({.isHeader = false, .group = groupId, .text = ""});
            }
            lines.push_back({
                .isHeader = true,
                .group = groupId,
                .text = std::format("<span foreground='#f5c96b' weight='bold'>{}</span>", escape(dg.title)),
            });
            for (const auto* row : dg.rows) {
              std::string escapedChord = escape(row->chord);
              const bool isDitto = row->action == "\xe2\x80\xb3";
              std::string escapedAction = escape(isDitto ? row->action : row->action);
              size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
              std::string padding(extraPad, ' ');
              const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
              lines.push_back({
                  .isHeader = false,
                  .isDitto = isDitto,
                  .group = groupId,
                  .text = std::format(
                      "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span foreground='{}'>{}</span>",
                      escapedChord, padding, actionColor, escapedAction
                  ),
              });
            }
          }
          continue;
        }

        // All other groups: flat row rendering.
        for (const auto* row : groupRows) {
          std::string escapedChord = escape(row->chord);
          std::string escapedAction = escape(row->action);
          size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
          std::string padding(extraPad, ' ');
          const bool isDitto = row->action == "\xe2\x80\xb3";
          const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
          lines.push_back({
              .isHeader = false,
              .isDitto = isDitto,
              .group = groupId,
              .text = std::format(
                  "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span foreground='{}'>{}</span>",
                  escapedChord, padding, actionColor, escapedAction
              ),
          });
        }
      }

      // Submap groups.
      for (const auto& smName : submapOrder) {
        std::vector<const CheatsheetRow*> groupRows;
        for (const auto& row : rows) {
          if (row.submap == smName) {
            groupRows.push_back(&row);
          }
        }
        if (groupRows.empty())
          continue;

        ++groupId;
        if (!lines.empty()) {
          lines.push_back({.isHeader = false, .group = groupId, .text = ""});
        }
        lines.push_back({
            .isHeader = true,
            .group = groupId,
            .text = std::format("<span foreground='#f5c96b' weight='bold'>Submap: {}</span>", escape(smName)),
        });

        size_t maxChordLen = 0;
        for (const auto* row : groupRows) {
          maxChordLen = std::max(maxChordLen, row->chord.size());
        }

        for (const auto* row : groupRows) {
          std::string escapedChord = escape(row->chord);
          std::string escapedAction = escape(row->action);
          size_t extraPad = maxChordLen > row->chord.size() ? maxChordLen - row->chord.size() : 0;
          std::string padding(extraPad, ' ');
          const bool isDitto = row->action == "\xe2\x80\xb3";
          const char* actionColor = isDitto ? "#8a8a92" : "#e8e8ea";
          lines.push_back({
              .isHeader = false,
              .isDitto = isDitto,
              .group = groupId,
              .text = std::format(
                  "<span background='#26262e' foreground='#bfd3ff'> {} </span>{}  <span foreground='{}'>{}</span>",
                  escapedChord, padding, actionColor, escapedAction
              ),
          });
        }
      }

      return lines;
    };

    auto allLines = buildLines();

    // --- Handle empty bind list ---
    if (allLines.empty()) {
      allLines.push_back({
          .isHeader = false,
          .text = "<span foreground='#8a8a92'>no keybinds configured</span>",
      });
    }

    // --- Step 4: Column layout ---
    auto layoutColumns = [&](int numCols) -> std::vector<std::string> {
      const int lineCount = static_cast<int>(allLines.size());
      const int linesPerCol = (lineCount + numCols - 1) / numCols;
      std::vector<std::string> columns;

      // Collect group boundary indices (first line of each group).
      std::vector<int> groupStarts;
      if (lineCount > 0) {
        groupStarts.push_back(0);
        for (int i = 1; i < lineCount; ++i) {
          if (allLines[static_cast<size_t>(i)].group != allLines[static_cast<size_t>(i - 1)].group) {
            groupStarts.push_back(i);
          }
        }
      }

      int pos = 0;
      for (int col = 0; col < numCols && pos < lineCount; ++col) {
        std::string markup;
        int colEnd = std::min(pos + linesPerCol, lineCount);

        // Snap colEnd back to the nearest group boundary so no group is split.
        if (colEnd < lineCount) {
          // Find the largest group start <= colEnd.
          auto it = std::ranges::upper_bound(groupStarts, colEnd);
          if (it != groupStarts.begin()) {
            --it;
            // Only pull back if it doesn't collapse the column to nothing.
            if (*it > pos) {
              colEnd = *it;
            }
          }
        }

        for (int i = pos; i < colEnd; ++i) {
          if (i > pos) {
            markup += '\n';
          }
          markup += allLines[static_cast<size_t>(i)].text;
        }
        if (!markup.empty()) {
          columns.push_back(std::move(markup));
        }
        pos = colEnd;
      }

      // Remaining lines go into the last column.
      if (pos < lineCount && !columns.empty()) {
        auto& last = columns.back();
        for (int i = pos; i < lineCount; ++i) {
          last += '\n';
          last += allLines[static_cast<size_t>(i)].text;
        }
      }

      return columns;
    };

    int lineCount = static_cast<int>(allLines.size());
    int numCols = std::clamp((lineCount + 17) / 18, 1, 3);

    // --- Step 5: Header/footer buffers ---
    std::string titleMarkup = "<span size='14pt' weight='bold' foreground='#7aa3ff'>Umbriel keybinds</span>";
    if (configFileMissing()) {
      titleMarkup += "\n<span foreground='#f5c96b'>no config found \xc2\xb7 showing built-in defaults</span>";
      titleMarkup += "\n<span foreground='#8a8a92'>copy example.toml to ~/.config/umbriel/config.toml</span>";
    }

    const char* modName = m_server.nested() ? "Alt" : "Super";
    std::string footerMarkup =
        std::format("<span foreground='#8a8a92'>Mod = {} \xc2\xb7 press any key to close</span>", modName);

    // Render title and footer.
    TextBufferResult titleBuf = renderTextBuffer({
        .markup = titleMarkup,
        .font = "monospace 11",
        .maxWidth = 900,
        .padding = 0,
        .scale = scale,
        .bgR = 0.0,
        .bgG = 0.0,
        .bgB = 0.0,
        .bgA = 0.0,
    });
    TextBufferResult footerBuf = renderTextBuffer({
        .markup = footerMarkup,
        .font = "monospace 11",
        .maxWidth = 900,
        .padding = 0,
        .scale = scale,
        .bgR = 0.0,
        .bgG = 0.0,
        .bgB = 0.0,
        .bgA = 0.0,
    });

    // Render columns, potentially retrying with more columns if too tall.
    auto renderColumns = [&](int cols) -> std::pair<std::vector<TextBufferResult>, int> {
      auto colMarkups = layoutColumns(cols);
      std::vector<TextBufferResult> buffers;
      int maxH = 0;
      for (auto& markup : colMarkups) {
        TextBufferResult buf = renderTextBuffer({
            .markup = std::move(markup),
            .font = "monospace 11",
            .maxWidth = kColumnMaxWidth,
            .padding = 0,
            .scale = scale,
            .bgR = 0.0,
            .bgG = 0.0,
            .bgB = 0.0,
            .bgA = 0.0,
        });
        maxH = std::max(maxH, buf.logicalHeight);
        buffers.push_back(buf);
      }
      return {std::move(buffers), maxH};
    };

    auto [colBufs, maxColH] = renderColumns(numCols);

    // Check if panel exceeds output height.
    int totalH = titleBuf.logicalHeight + kTitleBodyGap + maxColH + kBodyFooterGap + footerBuf.logicalHeight + 2 * kPad;
    if (haveOutput && totalH > outputBox.height - 120 && numCols < kMaxColumns) {
      // Drop previous column buffers.
      for (auto& buf : colBufs) {
        if (buf.buffer != nullptr)
          wlr_buffer_drop(buf.buffer);
      }
      numCols = std::min(numCols + 1, kMaxColumns);
      auto [newBufs, newMaxH] = renderColumns(numCols);
      colBufs = std::move(newBufs);
      maxColH = newMaxH;
      totalH = titleBuf.logicalHeight + kTitleBodyGap + maxColH + kBodyFooterGap + footerBuf.logicalHeight + 2 * kPad;
    }

    // --- Step 6: Panel assembly ---
    int totalColW = 0;
    for (size_t i = 0; i < colBufs.size(); ++i) {
      totalColW += colBufs[i].logicalWidth;
      if (i + 1 < colBufs.size()) {
        totalColW += kColumnGap;
      }
    }

    int panelW = std::max({titleBuf.logicalWidth, footerBuf.logicalWidth, totalColW}) + 2 * kPad;
    int panelH = totalH;

    // Shadow.
    m_shadow.update(m_tree, panelW, panelH, 0, kCornerRadius, nullptr);

    // Panel rect.
    wlr_scene_rect* panelRect = wlr_scene_rect_create(m_tree, panelW, panelH, kPanelColor);
    wlr_scene_rect_set_corner_radius(panelRect, kCornerRadius);
    (void)panelRect;

    // Helper to add a text buffer to the scene tree.
    auto addBuffer = [this](TextBufferResult& result, int x, int y) {
      if (result.buffer == nullptr)
        return;
      wlr_scene_buffer* sceneBuf = wlr_scene_buffer_create(m_tree, result.buffer);
      wlr_buffer_drop(result.buffer);
      result.buffer = nullptr;
      if (sceneBuf == nullptr)
        return;
      wlr_scene_buffer_set_dest_size(sceneBuf, result.logicalWidth, result.logicalHeight);
      sceneBuf->point_accepts_input = [](wlr_scene_buffer*, double*, double*) -> bool { return false; };
      wlr_scene_node_set_position(&sceneBuf->node, x, y);
    };

    // Title.
    int curY = kPad;
    addBuffer(titleBuf, kPad, curY);
    curY += titleBuf.logicalHeight + kTitleBodyGap;

    // Columns.
    int colX = kPad;
    for (auto& buf : colBufs) {
      addBuffer(buf, colX, curY);
      colX += buf.logicalWidth + kColumnGap;
    }
    curY += maxColH + kBodyFooterGap;

    // Footer.
    addBuffer(footerBuf, kPad, curY);

    // Position: centered on preferred output.
    if (haveOutput) {
      int x = outputBox.x + (outputBox.width - panelW) / 2;
      int y = outputBox.y + (outputBox.height - panelH) / 2;
      wlr_scene_node_set_position(&m_tree->node, x, y);
    } else {
      wlr_scene_node_set_position(&m_tree->node, 24, 24);
    }
  }

} // namespace umbriel
