#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct wlr_box;

namespace umbriel {

  class View;
  class Layout;
  struct ResolvedLayoutConfig;

  enum class LayoutMode {
    Scrolling,
    Dwindle,
  };

  struct Column {
    std::vector<View*> views;
    std::vector<double> heightWeights;
    double topGapWeight = 0.0;
    double bottomGapWeight = 0.0;
    double widthFrac = 0.5;
    double savedWidthFrac = 0.0;
  };

  // Layout-owned interactive resize session. Cursor feeds a pointer delta; the
  // session mutates only its layout's geometry state (split ratios, width
  // fractions, row weights). Protocol calls and arrange() stay in Cursor.
  struct ResizeGrab {
    virtual ~ResizeGrab() = default;
    virtual void applyDelta(double dx, double dy, const wlr_box& usable) = 0;
    // Identity of the layout that created this session, for stale-pointer
    // detection when a config reload swaps the workspace's layout mid-grab.
    [[nodiscard]] virtual const Layout* ownerLayout() const = 0;
    // True when the layout cleared a maximized/full-width state at grab start,
    // so Cursor should un-maximize the toplevel immediately (matches legacy).
    [[nodiscard]] virtual bool unmaximizeOnBegin() const { return false; }
  };

  class Layout {
  public:
    virtual ~Layout() = default;
    void setConfig(const ResolvedLayoutConfig* config) { m_config = config; }
    [[nodiscard]] const ResolvedLayoutConfig* layoutConfig() const { return m_config; }

    [[nodiscard]] virtual LayoutMode mode() const = 0;

    [[nodiscard]] virtual int columnOf(const View* view) const = 0;
    [[nodiscard]] virtual int rowOf(const View* view) const = 0;

    virtual void insertView(View* view, int columnIndex) = 0;
    virtual void insertViewIntoColumn(View* view, int columnIndex, int rowIndex) = 0;
    virtual bool consumeLeft(View* view) = 0;
    virtual bool expelRight(View* view) = 0;
    virtual bool moveViewVertical(View* view, int direction) = 0;
    virtual void removeView(View* view) = 0;
    virtual void moveColumn(int from, int to) = 0;
    virtual void arrange(const wlr_box& usable) = 0;

    [[nodiscard]] virtual wlr_box targetBox(const View* view) const = 0;

    [[nodiscard]] virtual View* focusVerticalLeaf(const View* /*view*/, int /*direction*/) const { return nullptr; }

    virtual bool cycleWidth(int columnIndex) = 0;
    virtual bool toggleFullWidth(int columnIndex) = 0;
    virtual bool setWidthFraction(int columnIndex, double fraction) = 0;
    virtual void clearFullWidthState(int columnIndex) = 0;
    [[nodiscard]] virtual double widthFraction(int columnIndex) const = 0;

    // ---- Interactive resize ----
    // Edges grabbable at a pointer position (0 = none). Base = not resizable.
    [[nodiscard]] virtual uint32_t resizeEdgesAt(const View* /*view*/, double /*cx*/, double /*cy*/) const { return 0; }
    // Drop edges this layout can't resize (e.g. solo scrolling column vertical). Base = unchanged.
    [[nodiscard]] virtual uint32_t sanitizeResizeEdges(const View* /*view*/, uint32_t edges) const { return edges; }
    // Resolve the final resize edges: an explicit request is sanitized; an empty
    // request (or one sanitized to nothing) falls back to the pointer proposal.
    [[nodiscard]] uint32_t resolveResizeEdges(const View* view, uint32_t requested, double cx, double cy) const {
      uint32_t edges = requested != 0 ? sanitizeResizeEdges(view, requested) : resizeEdgesAt(view, cx, cy);
      if (edges == 0) {
        edges = resizeEdgesAt(view, cx, cy);
      }
      return edges;
    }
    // Begin an interactive resize with already-resolved edges; null = not resizable.
    virtual std::unique_ptr<ResizeGrab> beginResize(View* /*view*/, uint32_t /*edges*/, const wlr_box& /*usable*/) {
      return nullptr;
    }

    // ---- Scrolling-specific (default no-ops for non-scrolling layouts) ----

    [[nodiscard]] virtual const std::vector<Column>& columns() const {
      static const std::vector<Column> empty;
      return empty;
    }

    [[nodiscard]] virtual double scroll() const { return 0; }
    virtual void setScroll(double) {}
    [[nodiscard]] virtual int maxScroll(int) const { return 0; }
    virtual void ensureVisible(int, int) {}
    [[nodiscard]] virtual double scrollAmountToEnsureVisible(int, int) const { return 0; }

    [[nodiscard]] virtual bool isFullWidth(int) const { return false; }
    [[nodiscard]] virtual int columnX(int, int) const { return 0; }
    [[nodiscard]] virtual int columnWidth(int, int) const { return 0; }

    virtual bool setRowBoundary(int, int, double, double) { return false; }
    virtual bool setHeightWeight(int, int, double) { return false; }
    virtual bool setTopGapWeight(int, double) { return false; }
    virtual bool setBottomGapWeight(int, double) { return false; }
    [[nodiscard]] virtual double heightWeight(int, int) const { return 1.0; }
    [[nodiscard]] virtual double topGapWeight(int) const { return 0.0; }
    [[nodiscard]] virtual double bottomGapWeight(int) const { return 0.0; }

  protected:
    const ResolvedLayoutConfig* m_config = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Layout> createLayout(LayoutMode mode);

} // namespace umbriel
