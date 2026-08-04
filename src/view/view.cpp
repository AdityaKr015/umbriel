#include "view/view.h"

#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/scrolling.h"
#include "output/output.h"
#include "scene/color.h"
#include "server/server.h"
#include "view/xdg_size.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {
  struct View::BorderEdge {
    wlr_box box;
    fx_corner_radii outer;
    bool hasHole;
    wlr_box hole;
    fx_corner_radii holeCorners;
  };

  namespace {
    bool looksTiled(const wlr_xdg_toplevel* toplevel) {
      const auto& state = toplevel->current;
      const bool fixedWidth = state.max_width > 0 && state.min_width == state.max_width;
      const bool fixedHeight = state.max_height > 0 && state.min_height == state.max_height;
      return toplevel->parent == nullptr && !fixedWidth && !fixedHeight;
    }

    int expandedRadius(int radius, int thickness) { return radius > 0 ? radius + thickness : 0; }
  } // namespace

  std::array<View::BorderEdge, 4> View::makeBorderRing(int contentWidth, int contentHeight, int radius, int thickness) {
    const int width = contentWidth + 2 * thickness;
    const int innerWidth = std::max(0, width - 2 * thickness);
    const int sideHeight = std::max(0, contentHeight - 2 * radius);
    const int outer = expandedRadius(radius, thickness);
    return {{
        {
            .box = {-thickness, -thickness, width, thickness + radius},
            .outer = corner_radii_top(outer),
            .hasHole = true,
            .hole = {thickness, thickness, innerWidth, thickness + radius},
            .holeCorners = corner_radii_top(radius),
        },
        {
            .box = {-thickness, contentHeight - radius, width, thickness + radius},
            .outer = corner_radii_bottom(outer),
            .hasHole = true,
            .hole = {thickness, -1, innerWidth, radius + 1},
            .holeCorners = corner_radii_bottom(radius),
        },
        {
            .box = {-thickness, radius, thickness, sideHeight},
            .outer = corner_radii_none(),
            .hasHole = false,
            .hole = {},
            .holeCorners = corner_radii_none(),
        },
        {
            .box = {contentWidth, radius, thickness, sideHeight},
            .outer = corner_radii_none(),
            .hasHole = false,
            .hole = {},
            .holeCorners = corner_radii_none(),
        },
    }};
  }

  View::View(Server& server, wlr_xdg_toplevel* toplevel)
      : SceneNode(SceneNodeKind::View), m_server(&server), m_toplevel(toplevel) {
    // Register map/unmap listeners BEFORE creating the scene tree so our
    // handlers fire before wlroots' internal unmap handler disables the
    // surface subtree (needed for close-animation buffer snapshot).
    m_map.notify = onMap;
    wl_signal_add(&m_toplevel->base->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_toplevel->base->surface->events.unmap, &m_unmap);

    m_sceneTree = wlr_scene_xdg_surface_create(m_server->xdgTree(), m_toplevel->base);
    m_sceneTree->node.data = this;
    m_toplevel->base->data = m_sceneTree;
    wlr_scene_node_set_enabled(&m_sceneTree->node, false);
    if (wlr_output* output = m_server->preferredOutput()) {
      wlr_surface* surface = m_toplevel->base->surface;
      wlr_fractional_scale_v1_notify_scale(surface, output->scale);
      wlr_surface_set_preferred_buffer_scale(surface, static_cast<int32_t>(std::ceil(output->scale)));
    }

    m_commit.notify = onCommit;
    wl_signal_add(&m_toplevel->base->surface->events.commit, &m_commit);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_toplevel->events.destroy, &m_destroy);

    m_requestMove.notify = onRequestMove;
    wl_signal_add(&m_toplevel->events.request_move, &m_requestMove);
    m_requestResize.notify = onRequestResize;
    wl_signal_add(&m_toplevel->events.request_resize, &m_requestResize);
    m_requestMaximize.notify = onRequestMaximize;
    wl_signal_add(&m_toplevel->events.request_maximize, &m_requestMaximize);
    m_requestFullscreen.notify = onRequestFullscreen;
    wl_signal_add(&m_toplevel->events.request_fullscreen, &m_requestFullscreen);
    m_setTitle.notify = onSetTitle;
    wl_signal_add(&m_toplevel->events.set_title, &m_setTitle);
    m_setAppId.notify = onSetAppId;
    wl_signal_add(&m_toplevel->events.set_app_id, &m_setAppId);

    if (wlr_foreign_toplevel_manager_v1* manager = m_server->foreignToplevelManager()) {
      m_foreign = wlr_foreign_toplevel_handle_v1_create(manager);
      if (m_foreign != nullptr) {
        m_foreign->data = this;
        m_foreignActivate.notify = onForeignActivate;
        wl_signal_add(&m_foreign->events.request_activate, &m_foreignActivate);
        m_foreignClose.notify = onForeignClose;
        wl_signal_add(&m_foreign->events.request_close, &m_foreignClose);
        m_foreignDestroy.notify = onForeignDestroy;
        wl_signal_add(&m_foreign->events.destroy, &m_foreignDestroy);
        updateForeignIdentity();
        updateForeignState();
      }
    }
  }

  View::~View() {
    if (m_fadeAnim != 0) {
      m_server->animator().cancel(m_fadeAnim);
      m_fadeAnim = 0;
    }
    if (m_sizeAnim != 0) {
      m_server->animator().cancel(m_sizeAnim);
      m_sizeAnim = 0;
    }
    cancelPositionAnimation();
    setWorkspace(nullptr);
    if (m_map.link.next != nullptr) {
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_destroy.link);
      wl_list_remove(&m_requestMove.link);
      wl_list_remove(&m_requestResize.link);
      wl_list_remove(&m_requestMaximize.link);
      wl_list_remove(&m_requestFullscreen.link);
      wl_list_remove(&m_setTitle.link);
      wl_list_remove(&m_setAppId.link);
    }
    if (m_foreign != nullptr) {
      leaveForeignOutput();
      wlr_foreign_toplevel_handle_v1_destroy(m_foreign);
      m_foreign = nullptr;
    }
  }

  void View::setWorkspace(Workspace* workspace) {
    if (m_workspace == workspace) {
      return;
    }
    if (m_workspace != nullptr) {
      m_workspace->removeView(this);
    }
    m_workspace = workspace;
    if (m_workspace != nullptr) {
      m_workspace->addView(this);
    } else {
      setOnActiveWorkspace(true);
    }
    if (m_mapped && m_onActiveWorkspace) {
      enterForeignOutput();
    }
  }

  void View::detachWorkspace() {
    m_workspace = nullptr;
    setOnActiveWorkspace(true);
  }

  void View::setOnActiveWorkspace(bool active) {
    if (m_onActiveWorkspace == active) {
      return;
    }
    m_onActiveWorkspace = active;
    if (m_sceneTree != nullptr) {
      wlr_scene_node_set_enabled(&m_sceneTree->node, active);
    }
    if (!m_mapped) {
      return;
    }
    if (active) {
      enterForeignOutput();
    } else {
      leaveForeignOutput();
      setForeignActivated(false);
      setBorderFocused(false);
    }
  }

  void View::applySeatFocus() {
    // Mechanism only. Policy lives in Server::focusView — do not call directly
    // from input/event code.
    if (!m_onActiveWorkspace) {
      return;
    }

    wlr_seat* seat = m_server->seat()->wlr();
    wlr_surface* surface = m_toplevel->base->surface;
    // Always clear other views first: the previous seat surface may be a layer, so
    // deactivating only that surface can leave another window's focus border on.
    m_server->deactivateViews(this);

    wlr_scene_node_raise_to_top(&m_sceneTree->node);
    wlr_xdg_toplevel_set_activated(m_toplevel, true);
    setBorderFocused(true);
    setForeignActivated(true);

    if (seat->keyboard_state.focused_surface == surface) {
      return;
    }
    if (wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat)) {
      wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
  }

  void View::cancelPositionAnimation() {
    if (m_posAnim != 0) {
      m_server->animator().cancel(m_posAnim);
      m_posAnim = 0;
    }
  }

  void View::setFadeAlpha(float alpha) {
    m_fadeAlpha = alpha;
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          wlr_scene_buffer_set_opacity(buffer, *static_cast<float*>(data));
        },
        &m_fadeAlpha
    );
    setBorderFocused(m_borderFocusedState);
    m_shadow.setAlpha(alpha);
    if (alpha < 1.0F) {
      m_blur.hide();
    } else {
      updateBlur();
    }
  }

  void View::cancelFadeAnimation() {
    if (m_fadeAnim != 0) {
      m_server->animator().cancel(m_fadeAnim);
      m_fadeAnim = 0;
    }
    setFadeAlpha(1.0F);
  }

  int View::presentedWidth(const wlr_box& target) const {
    if (m_sizeAnim != 0) {
      return m_presentedW;
    }
    return std::min(m_toplevel->base->geometry.width, target.width);
  }

  int View::presentedHeight(const wlr_box& target) const {
    if (m_sizeAnim != 0) {
      return m_presentedH;
    }
    return std::min(m_toplevel->base->geometry.height, target.height);
  }

  void View::applyPresentedSize() {
    // Scale the toplevel's primary surface buffer to the animated size.
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          auto* self = static_cast<View*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr || sceneSurface->surface != self->m_toplevel->base->surface) {
            return;
          }
          const wlr_box& geo = self->m_toplevel->base->geometry;
          if (geo.width > 0 && geo.height > 0) {
            wlr_scene_buffer_set_dest_size(
                buffer,
                std::max(
                    1,
                    static_cast<int>(std::lround(
                        sceneSurface->surface->current.width * static_cast<double>(self->m_presentedW) / geo.width
                    ))
                ),
                std::max(
                    1,
                    static_cast<int>(std::lround(
                        sceneSurface->surface->current.height * static_cast<double>(self->m_presentedH) / geo.height
                    ))
                )
            );
          }
        },
        this
    );
    updateBorderGeometry(m_presentedW, m_presentedH);
    // Shadow with presented size.
    if (!m_toplevel->scheduled.fullscreen) {
      const bool decorated = m_borderTree != nullptr && m_borderTree->node.enabled;
      const int total = decorated ? config().appearance.totalBorderWidth() : 0;
      const int radius = decorated ? expandedRadius(config().appearance.cornerRadius, total) : 0;
      m_shadow.update(m_sceneTree, m_presentedW, m_presentedH, total, radius);
    }
    // Blur with presented size.
    const wlr_box nodeBox{0, 0, m_presentedW, m_presentedH};
    const bool rounded = m_borderTree != nullptr && m_borderTree->node.enabled && !m_toplevel->scheduled.fullscreen;
    m_blur.update(
        m_sceneTree, m_toplevel->base->surface, nodeBox, m_toplevel->base->geometry,
        rounded ? config().appearance.cornerRadius : 0
    );
    if (m_workspace != nullptr) {
      m_workspace->syncViewPresentation(this);
    }
  }

  void View::cancelSizeAnimation() {
    if (m_sizeAnim != 0) {
      m_server->animator().cancel(m_sizeAnim);
      m_sizeAnim = 0;
      const wlr_box& geo = m_toplevel->base->geometry;
      m_presentedW = geo.width;
      m_presentedH = geo.height;
      // Restore the primary surface buffer to its real size.
      wlr_scene_node_for_each_buffer(
          &m_sceneTree->node,
          [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
            auto* self = static_cast<View*>(data);
            wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
            if (sceneSurface == nullptr || sceneSurface->surface != self->m_toplevel->base->surface) {
              return;
            }
            wlr_scene_buffer_set_dest_size(
                buffer, sceneSurface->surface->current.width, sceneSurface->surface->current.height
            );
          },
          this
      );
      updateBorderGeometry();
      updateBlur();
      updateShadow();
      if (m_workspace != nullptr) {
        m_workspace->syncViewPresentation(this);
      }
    }
  }

  void View::setPosition(int x, int y) {
    cancelPositionAnimation();
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
  }

  void View::animateTo(int x, int y) {
    if (!m_mapped || !m_onActiveWorkspace) {
      setPosition(x, y);
      return;
    }
    const int fromX = m_sceneTree->node.x;
    const int fromY = m_sceneTree->node.y;
    if (fromX == x && fromY == y) {
      cancelPositionAnimation();
      return;
    }
    cancelPositionAnimation();
    m_posAnim = m_server->animator().animate(
        0.0, 1.0, config().appearance.animationMs, Easing::EaseOutCubic,
        [this, fromX, fromY, x, y](double progress) {
          wlr_scene_node_set_position(
              &m_sceneTree->node, static_cast<int>(std::lround(fromX + (x - fromX) * progress)),
              static_cast<int>(std::lround(fromY + (y - fromY) * progress))
          );
        },
        [this] { m_posAnim = 0; }
    );
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
    }
  }

  void View::onMap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_map);
    self->handleMap();
  }

  void View::onUnmap(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_unmap);
    self->handleUnmap();
  }

  void View::onCommit(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_commit);
    self->handleCommit();
  }

  void View::onDestroy(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void View::onRequestMove(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestMove);
    self->handleRequestMove();
  }

  void View::onRequestResize(wl_listener* listener, void* data) {
    View* self = wl_container_of(listener, self, m_requestResize);
    self->handleRequestResize(data);
  }

  void View::onRequestMaximize(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestMaximize);
    self->handleRequestMaximize();
  }

  void View::onRequestFullscreen(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_requestFullscreen);
    self->handleRequestFullscreen();
  }

  void View::onSetTitle(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setTitle);
    self->handleSetTitle();
  }

  void View::onSetAppId(wl_listener* listener, void* /*data*/) {
    View* self = wl_container_of(listener, self, m_setAppId);
    self->handleSetAppId();
  }

  void View::placeInUsableArea() {
    wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
    if (usable.width <= 0 || usable.height <= 0) {
      return;
    }

    // Floats keep their own size; only center within the usable area.
    const wlr_box& geo = m_toplevel->base->geometry;
    const int width = geo.width > 0 ? geo.width : usable.width;
    const int height = geo.height > 0 ? geo.height : usable.height;
    const int x = usable.x + (usable.width - width) / 2;
    const int y = usable.y + (usable.height - height) / 2;
    wlr_scene_node_set_position(&m_sceneTree->node, x, y);
  }

  std::array<View::BorderEdge, 4> View::borderEdges() const {
    const wlr_box& geometry = m_toplevel->base->geometry;
    return borderEdges(geometry.width, geometry.height);
  }

  std::array<View::BorderEdge, 4> View::borderEdges(int contentWidth, int contentHeight) const {
    return makeBorderRing(
        contentWidth, contentHeight, config().appearance.cornerRadius, config().appearance.borderWidth
    );
  }

  void View::updateBorderGeometry() {
    const wlr_box& geometry = m_toplevel->base->geometry;
    updateBorderGeometry(geometry.width, geometry.height);
  }

  void View::updateBorderGeometry(int contentWidth, int contentHeight) {
    if (m_borderTree == nullptr) {
      return;
    }
    const auto edges = borderEdges(contentWidth, contentHeight);
    for (size_t i = 0; i < edges.size(); ++i) {
      wlr_scene_rect* rect = m_borderRects[i];
      if (rect == nullptr) {
        continue;
      }
      const BorderEdge& edge = edges[i];
      wlr_scene_node_set_position(&rect->node, edge.box.x, edge.box.y);
      wlr_scene_rect_set_size(rect, edge.box.width, edge.box.height);
      wlr_scene_rect_set_corner_radii(rect, edge.outer);
      wlr_scene_rect_set_clipped_region(
          rect,
          edge.hasHole ? clipped_region{.area = edge.hole, .corners = edge.holeCorners} : clipped_region_get_default()
      );
    }

    if (m_outerBorderRect != nullptr) {
      const int outer = config().appearance.outerBorderWidth;
      const int total = config().appearance.totalBorderWidth();
      const int radius = config().appearance.cornerRadius;
      if (outer <= 0) {
        wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
      } else {
        // Fill the full decoration bounds; hole is only the window surface so the
        // outer color tucks under the inner border (no gap between the two rings).
        wlr_scene_node_set_position(&m_outerBorderRect->node, -total, -total);
        wlr_scene_rect_set_size(m_outerBorderRect, contentWidth + 2 * total, contentHeight + 2 * total);
        const int outerRadius = expandedRadius(radius, total);
        wlr_scene_rect_set_corner_radii(
            m_outerBorderRect, corner_radii_new(outerRadius, outerRadius, outerRadius, outerRadius)
        );
        wlr_scene_rect_set_color(m_outerBorderRect, config().appearance.outerBorderColor.data());
        wlr_scene_rect_set_clipped_region(
            m_outerBorderRect,
            clipped_region{
                .area = {total, total, contentWidth, contentHeight},
                .corners = corner_radii_new(radius, radius, radius, radius),
            }
        );
      }
    }

    for (wlr_scene_rect* rect : m_borderRects) {
      if (rect != nullptr) {
        wlr_scene_node_raise_to_top(&rect->node);
      }
    }
  }

  void View::setBorderFocused(bool focused) {
    m_borderFocusedState = focused;
    if (m_borderTree == nullptr) {
      return;
    }
    const auto& baseColor = focused ? config().appearance.borderFocused : config().appearance.borderUnfocused;
    float color[4];
    premultiplied(color, baseColor, m_fadeAlpha);
    for (wlr_scene_rect* rect : m_borderRects) {
      if (rect == nullptr) {
        continue;
      }
      wlr_scene_rect_set_color(rect, color);
    }
    if (m_outerBorderRect != nullptr) {
      float outerColor[4];
      premultiplied(outerColor, config().appearance.outerBorderColor, m_fadeAlpha);
      wlr_scene_rect_set_color(m_outerBorderRect, outerColor);
    }
  }

  void
  View::applyOuterBorderClip(const wlr_box& target, const wlr_box& outputBox, int contentWidth, int contentHeight) {
    if (m_outerBorderRect == nullptr || config().appearance.outerBorderWidth <= 0) {
      if (m_outerBorderRect != nullptr) {
        wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
      }
      return;
    }
    const int total = config().appearance.totalBorderWidth();
    const int radius = config().appearance.cornerRadius;
    const wlr_box screenBox{
        .x = target.x - total,
        .y = target.y - total,
        .width = contentWidth + 2 * total,
        .height = contentHeight + 2 * total,
    };
    wlr_box visible{};
    if (!wlr_box_intersection(&visible, &screenBox, &outputBox)) {
      wlr_scene_rect_set_size(m_outerBorderRect, 0, 0);
      return;
    }

    wlr_scene_node_set_position(&m_outerBorderRect->node, visible.x - target.x, visible.y - target.y);
    wlr_scene_rect_set_size(m_outerBorderRect, visible.width, visible.height);

    const bool trimLeft = visible.x > screenBox.x;
    const bool trimRight = visible.x + visible.width < screenBox.x + screenBox.width;
    const bool trimTop = visible.y > screenBox.y;
    const bool trimBottom = visible.y + visible.height < screenBox.y + screenBox.height;
    const int outerRadius = expandedRadius(radius, total);
    wlr_scene_rect_set_corner_radii(
        m_outerBorderRect,
        corner_radii_new(
            trimLeft || trimTop ? 0 : outerRadius, trimRight || trimTop ? 0 : outerRadius,
            trimRight || trimBottom ? 0 : outerRadius, trimLeft || trimBottom ? 0 : outerRadius
        )
    );

    // Hole matches the window surface; inner border covers the overlap on top.
    wlr_box hole{
        .x = screenBox.x + total - visible.x,
        .y = screenBox.y + total - visible.y,
        .width = contentWidth,
        .height = contentHeight,
    };
    wlr_scene_rect_set_clipped_region(
        m_outerBorderRect,
        clipped_region{
            .area = hole,
            .corners = corner_radii_new(
                trimLeft || trimTop ? 0 : radius, trimRight || trimTop ? 0 : radius,
                trimRight || trimBottom ? 0 : radius, trimLeft || trimBottom ? 0 : radius
            ),
        }
    );
  }

  void View::applyBorderClip(
      wlr_scene_rect* const rects[4], const std::array<BorderEdge, 4>& edges, const wlr_box& target,
      const wlr_box& outputBox
  ) {
    for (size_t i = 0; i < edges.size(); ++i) {
      wlr_scene_rect* rect = rects[i];
      if (rect == nullptr) {
        continue;
      }
      const BorderEdge& edge = edges[i];
      wlr_box screenBox = edge.box;
      screenBox.x += target.x;
      screenBox.y += target.y;

      wlr_box visible{};
      if (!wlr_box_intersection(&visible, &screenBox, &outputBox)) {
        wlr_scene_rect_set_size(rect, 0, 0);
        continue;
      }

      wlr_scene_node_set_position(&rect->node, visible.x - target.x, visible.y - target.y);
      wlr_scene_rect_set_size(rect, visible.width, visible.height);

      const bool trimLeft = visible.x > screenBox.x;
      const bool trimRight = visible.x + visible.width < screenBox.x + screenBox.width;
      const bool trimTop = visible.y > screenBox.y;
      const bool trimBottom = visible.y + visible.height < screenBox.y + screenBox.height;
      const auto trimCorners = [&](fx_corner_radii corners) {
        return corner_radii_new(
            trimLeft || trimTop ? 0 : corners.top_left, trimRight || trimTop ? 0 : corners.top_right,
            trimRight || trimBottom ? 0 : corners.bottom_right, trimLeft || trimBottom ? 0 : corners.bottom_left
        );
      };

      wlr_scene_rect_set_corner_radii(rect, trimCorners(edge.outer));
      if (edge.hasHole) {
        wlr_box hole = edge.hole;
        hole.x += screenBox.x - visible.x;
        hole.y += screenBox.y - visible.y;
        wlr_scene_rect_set_clipped_region(rect, clipped_region{.area = hole, .corners = trimCorners(edge.holeCorners)});
      } else {
        wlr_scene_rect_set_clipped_region(rect, clipped_region_get_default());
      }
    }
  }

  void View::applyCornerRadius() {
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
          auto* self = static_cast<View*>(data);
          wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
          if (sceneSurface == nullptr || sceneSurface->surface != self->m_toplevel->base->surface) {
            return;
          }

          const bool rounded = self->m_borderTree != nullptr
              && self->m_borderTree->node.enabled
              && !self->m_toplevel->scheduled.fullscreen;
          wlr_scene_buffer_set_corner_radius(buffer, rounded ? config().appearance.cornerRadius : 0);
        },
        this
    );
  }

  void View::updateBlur() {
    if (m_fadeAlpha < 1.0F) {
      m_blur.hide();
      return;
    }
    const wlr_box& geometry = m_toplevel->base->geometry;
    const wlr_box nodeBox{0, 0, geometry.width, geometry.height};
    const bool rounded = m_borderTree != nullptr && m_borderTree->node.enabled && !m_toplevel->scheduled.fullscreen;
    m_blur.update(
        m_sceneTree, m_toplevel->base->surface, nodeBox, geometry, rounded ? config().appearance.cornerRadius : 0
    );
  }

  void View::updateShadow() {
    if (m_toplevel->scheduled.fullscreen) {
      m_shadow.hide();
      return;
    }
    const wlr_box& geometry = m_toplevel->base->geometry;
    const bool decorated = m_borderTree != nullptr && m_borderTree->node.enabled;
    const int total = decorated ? config().appearance.totalBorderWidth() : 0;
    const int radius = decorated ? expandedRadius(config().appearance.cornerRadius, total) : 0;
    m_shadow.update(m_sceneTree, geometry.width, geometry.height, total, radius);
  }

  void View::beginCloseAnimation() {
    if (!m_mapped || !m_onActiveWorkspace || m_server->sessionLocked()) {
      return;
    }

    wlr_scene_tree* snap = wlr_scene_tree_create(m_server->xdgTree());
    if (snap == nullptr) {
      return;
    }
    wlr_scene_node_set_position(&snap->node, m_sceneTree->node.x, m_sceneTree->node.y);

    // Collect border rects for the snapshot.
    std::vector<std::pair<wlr_scene_rect*, std::array<float, 4>>> snapRects;

    if (m_borderTree != nullptr && m_borderTree->node.enabled) {
      const auto& focusedColor =
          m_borderFocusedState ? config().appearance.borderFocused : config().appearance.borderUnfocused;
      for (wlr_scene_rect* srcRect : m_borderRects) {
        if (srcRect == nullptr) {
          continue;
        }
        wlr_scene_rect* copy = wlr_scene_rect_create(snap, srcRect->width, srcRect->height, srcRect->color);
        if (copy == nullptr) {
          continue;
        }
        wlr_scene_node_set_position(
            &copy->node, m_borderTree->node.x + srcRect->node.x, m_borderTree->node.y + srcRect->node.y
        );
        wlr_scene_rect_set_corner_radii(copy, srcRect->corners);
        wlr_scene_rect_set_clipped_region(copy, srcRect->clipped_region);
        snapRects.push_back({copy, focusedColor});
      }
      if (m_outerBorderRect != nullptr && config().appearance.outerBorderWidth > 0) {
        wlr_scene_rect* copy =
            wlr_scene_rect_create(snap, m_outerBorderRect->width, m_outerBorderRect->height, m_outerBorderRect->color);
        if (copy != nullptr) {
          wlr_scene_node_set_position(
              &copy->node, m_borderTree->node.x + m_outerBorderRect->node.x,
              m_borderTree->node.y + m_outerBorderRect->node.y
          );
          wlr_scene_rect_set_corner_radii(copy, m_outerBorderRect->corners);
          wlr_scene_rect_set_clipped_region(copy, m_outerBorderRect->clipped_region);
          snapRects.push_back({copy, config().appearance.outerBorderColor});
        }
      }
    }

    // Copy surface buffers.
    struct CopyCtx {
      wlr_scene_tree* snap;
      int rootX;
      int rootY;
      int buffersCopied;
    };
    CopyCtx ctx{snap, m_sceneTree->node.x, m_sceneTree->node.y, 0};
    wlr_scene_node_for_each_buffer(
        &m_sceneTree->node,
        [](wlr_scene_buffer* src, int sx, int sy, void* data) {
          auto* c = static_cast<CopyCtx*>(data);
          if (src->buffer == nullptr || !src->node.enabled) {
            return;
          }
          wlr_scene_buffer* copy = wlr_scene_buffer_create(c->snap, src->buffer);
          if (copy == nullptr) {
            return;
          }
          wlr_scene_node_set_position(&copy->node, sx - c->rootX, sy - c->rootY);
          if (src->dst_width > 0 && src->dst_height > 0) {
            wlr_scene_buffer_set_dest_size(copy, src->dst_width, src->dst_height);
          }
          if (src->src_box.width > 0 && src->src_box.height > 0) {
            wlr_scene_buffer_set_source_box(copy, &src->src_box);
          }
          wlr_scene_buffer_set_transform(copy, src->transform);
          wlr_scene_buffer_set_corner_radii(copy, src->corners);
          wlr_scene_buffer_set_opacity(copy, src->opacity);
          ++c->buffersCopied;
        },
        &ctx
    );

    if (ctx.buffersCopied == 0) {
      wlr_scene_node_destroy(&snap->node);
      return;
    }

    m_server->animateCloseSnapshot(snap, std::move(snapRects));
    if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
      wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
    }
  }

  void View::clearOutputClip() {
    // Fullscreen must not keep a copied tile clip (that freezes usable-area size and
    // leaves a bar-sized gap). Use scheduled (not current): on leave, scheduled clears
    // immediately while current lags until the client acks.
    const bool fullscreen = m_toplevel->scheduled.fullscreen;
    if (!fullscreen && !m_tiled) {
      syncFloatingSurfaceClip();
      if (m_borderTree != nullptr) {
        updateBorderGeometry();
      }
      return;
    }
    const wlr_box* clip = (!fullscreen && m_tiled) ? &m_toplevel->base->geometry : nullptr;
    wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, clip);
    if (m_borderTree != nullptr) {
      updateBorderGeometry();
    }
    updateBlur();
    updateShadow();
  }

  void View::syncFloatingSurfaceClip() {
    if (m_tiled || m_toplevel->scheduled.fullscreen) {
      return;
    }
    const wlr_box& geo = m_toplevel->base->geometry;
    int width = m_toplevel->scheduled.width;
    int height = m_toplevel->scheduled.height;
    if (width <= 0) {
      width = m_toplevel->current.width;
    }
    if (height <= 0) {
      height = m_toplevel->current.height;
    }
    // Electron often keeps a wide buffer while tiled; without a clip, toggling float
    // would suddenly show the full surface.
    if (width > 0 && height > 0 && (geo.width > width || geo.height > height)) {
      const wlr_box clip{geo.x, geo.y, std::min(geo.width, width), std::min(geo.height, height)};
      wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, &clip);
    } else {
      wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, nullptr);
    }
    updateBlur();
    updateShadow();
  }

  wlr_scene_tree* View::homeTree() const {
    const bool fs = m_toplevel->scheduled.fullscreen;
    if (m_workspace != nullptr) {
      return fs ? m_workspace->fullscreenTree() : m_workspace->tree();
    }
    return fs ? m_server->fullscreenTree() : m_server->xdgTree();
  }

  void View::applyFullscreenLayout() {
    Output* output = nullptr;
    if (m_workspace != nullptr && m_workspace->group() != nullptr) {
      output = m_workspace->group()->output();
    }
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    wlr_output* wlrOutput = output != nullptr ? output->wlr() : m_server->preferredOutput();
    wlr_box fullArea{};
    wlr_output_layout_get_box(m_server->outputLayout(), wlrOutput, &fullArea);
    if (fullArea.width <= 0 || fullArea.height <= 0) {
      return;
    }
    if (m_toplevel->scheduled.width != fullArea.width || m_toplevel->scheduled.height != fullArea.height) {
      wlr_xdg_toplevel_set_size(m_toplevel, fullArea.width, fullArea.height);
    }
    wlr_scene_node_set_position(&m_sceneTree->node, fullArea.x, fullArea.y);
    wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, nullptr);
    updateBlur();
    updateShadow();
  }

  void View::setOutputClip(const wlr_box* screenIntersection, const wlr_box& target, const wlr_box& outputBox) {
    const wlr_box& geometry = m_toplevel->base->geometry;
    // Stay inside the tile while geometry lags configure (Electron often stays wide).
    const wlr_box content{
        .x = target.x,
        .y = target.y,
        .width = presentedWidth(target),
        .height = presentedHeight(target),
    };
    const int border = m_tiled ? config().appearance.totalBorderWidth() : 0;
    wlr_box decorated = content;
    decorated.x -= border;
    decorated.y -= border;
    decorated.width += 2 * border;
    decorated.height += 2 * border;

    wlr_box decoratedVisible{};
    if (screenIntersection == nullptr
        || !wlr_box_intersection(&decoratedVisible, &decorated, &outputBox)
        || decoratedVisible.width <= 0
        || decoratedVisible.height <= 0) {
      wlr_scene_node_set_enabled(&m_sceneTree->node, false);
      return;
    }

    wlr_box contentVisible{};
    const bool contentOnOutput = wlr_box_intersection(&contentVisible, &content, &outputBox);
    const bool decoratedFullyVisible = wlr_box_equal(&decoratedVisible, &decorated);

    if (contentOnOutput) {
      wlr_box surfaceClip{
          .x = geometry.x + contentVisible.x - content.x,
          .y = geometry.y + contentVisible.y - content.y,
          .width = contentVisible.width,
          .height = contentVisible.height,
      };
      // SceneFX is happier with even clip edges near output boundaries.
      if ((surfaceClip.x & 1) != 0) {
        --surfaceClip.x;
        ++surfaceClip.width;
      }
      if ((surfaceClip.width & 1) != 0) {
        ++surfaceClip.width;
      }
      // This also reaches popup subsurface trees, whose popup-local clip coordinates
      // can be wrong while the parent is partially off-output. This rare case is accepted.
      wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, &surfaceClip);
    } else {
      // Only the border/decoration remains on this output.
      const wlr_box empty{geometry.x, geometry.y, 0, 0};
      wlr_scene_subsurface_tree_set_clip(&m_sceneTree->node, &empty);
    }

    if (m_borderTree != nullptr) {
      if (decoratedFullyVisible) {
        updateBorderGeometry(content.width, content.height);
      } else {
        applyOuterBorderClip(target, outputBox, content.width, content.height);
        applyBorderClip(m_borderRects, borderEdges(content.width, content.height), target, outputBox);
        for (wlr_scene_rect* rect : m_borderRects) {
          if (rect != nullptr) {
            wlr_scene_node_raise_to_top(&rect->node);
          }
        }
      }
    }
    if (decoratedFullyVisible) {
      updateShadow();
    } else {
      m_shadow.hide();
    }

    const wlr_box nodeBox{0, 0, content.width, content.height};
    const bool rounded = m_borderTree != nullptr && m_borderTree->node.enabled && !m_toplevel->scheduled.fullscreen;
    if (!contentOnOutput || m_fadeAlpha < 1.0F) {
      m_blur.hide();
      return;
    }

    // Always clip blur to this output. SceneFX blur sampling can extend past the
    // node, so inset from output edges by the blur kernel to avoid neighbor bleed.
    const wlr_box outputLocal{
        .x = outputBox.x - target.x,
        .y = outputBox.y - target.y,
        .width = outputBox.width,
        .height = outputBox.height,
    };
    const wlr_box contentLocal{
        .x = contentVisible.x - target.x,
        .y = contentVisible.y - target.y,
        .width = contentVisible.width,
        .height = contentVisible.height,
    };
    wlr_box blurClip{};
    if (!wlr_box_intersection(&blurClip, &nodeBox, &contentLocal)
        || !wlr_box_intersection(&blurClip, &blurClip, &outputLocal)) {
      m_blur.hide();
      return;
    }
    const int bleed = std::max(0, config().appearance.blur.radius) * std::max(1, config().appearance.blur.passes);
    if (bleed > 0) {
      if (blurClip.x <= outputLocal.x) {
        blurClip.x += bleed;
        blurClip.width -= bleed;
      }
      if (blurClip.y <= outputLocal.y) {
        blurClip.y += bleed;
        blurClip.height -= bleed;
      }
      if (blurClip.x + blurClip.width >= outputLocal.x + outputLocal.width) {
        blurClip.width -= bleed;
      }
      if (blurClip.y + blurClip.height >= outputLocal.y + outputLocal.height) {
        blurClip.height -= bleed;
      }
    }
    if (blurClip.width <= 0 || blurClip.height <= 0) {
      m_blur.hide();
      return;
    }
    m_blur.update(
        m_sceneTree, m_toplevel->base->surface, nodeBox, geometry, rounded ? config().appearance.cornerRadius : 0,
        &blurClip
    );
  }

  void View::ensureBorders() {
    if (m_borderTree != nullptr) {
      return;
    }
    m_borderTree = wlr_scene_tree_create(m_sceneTree);
    // Outer below, then inner on top so the focus ring stays visible.
    m_outerBorderRect = wlr_scene_rect_create(m_borderTree, 0, 0, config().appearance.outerBorderColor.data());
    for (auto*& rect : m_borderRects) {
      rect = wlr_scene_rect_create(m_borderTree, 0, 0, config().appearance.borderUnfocused.data());
    }
    for (wlr_scene_rect* rect : m_borderRects) {
      wlr_scene_node_raise_to_top(&rect->node);
    }
    wlr_scene_node_lower_to_bottom(&m_borderTree->node);
  }

  void View::handleMap() {
    m_mapped = true;
    m_tiled = looksTiled(m_toplevel);
    const wlr_box& mapGeo = m_toplevel->base->geometry;
    m_presentedW = mapGeo.width;
    m_presentedH = mapGeo.height;
    clearOutputClip();
    if (m_tiled) {
      ensureBorders();
      wlr_scene_node_set_enabled(&m_borderTree->node, !m_toplevel->current.fullscreen);
      updateBorderGeometry();
    } else if (m_borderTree != nullptr) {
      wlr_scene_node_set_enabled(&m_borderTree->node, false);
    }
    applyCornerRadius();
    updateBlur();
    updateShadow();
    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(this);
    } else if (Output* out = m_server->outputFromWlr(m_server->preferredOutput())) {
      if (WorkspaceGroup* group = out->workspaceGroup()) {
        setWorkspace(group->active());
      } else {
        setOnActiveWorkspace(true);
      }
    } else {
      setOnActiveWorkspace(true);
    }
    if (!m_tiled) {
      placeInUsableArea();
    }
    updateForeignIdentity();
    updateForeignState();
    if (m_onActiveWorkspace && !m_server->sessionLocked()) {
      m_server->focusView(this);
    }
    if (m_onActiveWorkspace) {
      setFadeAlpha(0.0F);
      m_fadeAnim = m_server->animator().animate(
          0.0, 1.0, std::max(1, config().appearance.animationMs / 2), Easing::EaseOutCubic,
          [this](double v) { setFadeAlpha(static_cast<float>(v)); }, [this] { m_fadeAnim = 0; }
      );
      if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
        wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
      }
    }
  }

  void View::handleUnmap() {
    beginCloseAnimation();
    cancelFadeAnimation();
    cancelSizeAnimation();
    cancelPositionAnimation();
    if (m_borderTree != nullptr) {
      wlr_scene_node_set_enabled(&m_borderTree->node, false);
    }
    m_blur.hide();
    m_shadow.hide();
    if (m_toplevel->current.fullscreen || m_toplevel->scheduled.fullscreen) {
      // Move out of the fullscreen layer back to the normal workspace/xdg tree.
      wlr_scene_node_reparent(&m_sceneTree->node, m_workspace ? m_workspace->tree() : m_server->xdgTree());
    }
    m_mapped = false;
    if (m_workspace != nullptr) {
      m_workspace->layoutDetach(this);
    }
    leaveForeignOutput();
    setForeignActivated(false);
    if (m_server->cursor()->mode() != CursorMode::Passthrough) {
      m_server->cursor()->resetMode();
    }
  }

  void View::handleCommit() {
    if (m_toplevel->base->initial_commit) {
      if (looksTiled(m_toplevel)) {
        wlr_xdg_toplevel_set_tiled(m_toplevel, WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
        const wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
        const int viewportWidth = std::max(1, usable.width - 2 * config().layoutEdgePad());
        const int height = std::max(1, usable.height - 2 * config().layoutEdgePad());
        wlr_xdg_toplevel_set_size(
            m_toplevel,
            std::max(1, static_cast<int>(std::lround(config().layout.defaultWidthFraction * viewportWidth))), height
        );
      } else {
        wlr_xdg_toplevel_set_tiled(m_toplevel, 0);
        wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
      }
    }
    if (m_borderTree != nullptr && m_sizeAnim == 0) {
      const wlr_box& geometry = m_toplevel->base->geometry;
      if (m_borderRects[0]->width != geometry.width + 2 * config().appearance.borderWidth
          || m_borderRects[2]->height != std::max(0, geometry.height - 2 * config().appearance.cornerRadius)
          || (m_outerBorderRect != nullptr
              && m_outerBorderRect->width
                  != (config().appearance.outerBorderWidth > 0
                          ? geometry.width + 2 * config().appearance.totalBorderWidth()
                          : 0))) {
        updateBorderGeometry();
      }
    }
    applyCornerRadius();
    // Size animation trigger: animate presented size when the client's geometry
    // changes due to a layout resize (not during an interactive resize grab).
    if (m_mapped
        && m_tiled
        && m_onActiveWorkspace
        && m_workspace != nullptr
        && !m_toplevel->scheduled.fullscreen
        && !m_toplevel->current.fullscreen
        && m_presentedW > 0
        && m_presentedH > 0) {
      const wlr_box& geometry = m_toplevel->base->geometry;
      if (m_server->cursor()->mode() != CursorMode::Passthrough) {
        // During interactive resize, track geometry so no spurious animation
        // replays the drag when the grab ends and mode returns to Passthrough.
        if (geometry.width > 0 && geometry.height > 0) {
          m_presentedW = geometry.width;
          m_presentedH = geometry.height;
        }
      } else if (
          geometry.width > 0
          && geometry.height > 0
          && (geometry.width != m_presentedW || geometry.height != m_presentedH)
          && !(m_sizeAnim != 0 && geometry.width == m_sizeTargetW && geometry.height == m_sizeTargetH)
      ) {
        if (m_sizeAnim != 0) {
          m_server->animator().cancel(m_sizeAnim);
          m_sizeAnim = 0;
        }
        const int fromW = m_presentedW;
        const int fromH = m_presentedH;
        const int toW = geometry.width;
        const int toH = geometry.height;
        m_sizeTargetW = toW;
        m_sizeTargetH = toH;
        m_sizeAnim = m_server->animator().animate(
            0.0, 1.0, config().appearance.animationMs, Easing::EaseOutCubic,
            [this, fromW, fromH, toW, toH](double t) {
              m_presentedW = static_cast<int>(std::lround(fromW + (toW - fromW) * t));
              m_presentedH = static_cast<int>(std::lround(fromH + (toH - fromH) * t));
              applyPresentedSize();
            },
            [this] {
              m_sizeAnim = 0;
              const wlr_box& geo = m_toplevel->base->geometry;
              m_presentedW = geo.width;
              m_presentedH = geo.height;
              // Restore real surface size.
              wlr_scene_node_for_each_buffer(
                  &m_sceneTree->node,
                  [](wlr_scene_buffer* buffer, int /*sx*/, int /*sy*/, void* data) {
                    auto* self = static_cast<View*>(data);
                    wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
                    if (sceneSurface == nullptr || sceneSurface->surface != self->m_toplevel->base->surface) {
                      return;
                    }
                    wlr_scene_buffer_set_dest_size(
                        buffer, sceneSurface->surface->current.width, sceneSurface->surface->current.height
                    );
                  },
                  this
              );
              updateBorderGeometry();
              updateBlur();
              updateShadow();
              if (m_workspace != nullptr) {
                m_workspace->syncViewPresentation(this);
              }
            }
        );
        if (m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
          wlr_output_schedule_frame(m_workspace->group()->output()->wlr());
        }
      }
    }
    // Re-apply output clip after configure ack so Super+F / resize sizes show
    // without needing a workspace switch (clip boxes are copied, not live).
    if (m_mapped && m_tiled && m_workspace != nullptr && m_workspace->active()) {
      m_workspace->syncViewPresentation(this);
    } else if (m_mapped && !m_tiled) {
      syncFloatingSurfaceClip();
    } else {
      updateBlur();
      updateShadow();
    }
    updateForeignState();
  }

  void View::handleDestroy() {
    cancelFadeAnimation();
    cancelPositionAnimation();
    leaveForeignOutput();
    if (m_foreign != nullptr) {
      wl_list_remove(&m_foreignActivate.link);
      wl_list_remove(&m_foreignClose.link);
      wl_list_remove(&m_foreignDestroy.link);
      wlr_foreign_toplevel_handle_v1_destroy(m_foreign);
      m_foreign = nullptr;
    }

    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_destroy.link);
    wl_list_remove(&m_requestMove.link);
    wl_list_remove(&m_requestResize.link);
    wl_list_remove(&m_requestMaximize.link);
    wl_list_remove(&m_requestFullscreen.link);
    wl_list_remove(&m_setTitle.link);
    wl_list_remove(&m_setAppId.link);
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_commit.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_requestMove.link.next = nullptr;
    m_requestResize.link.next = nullptr;
    m_requestMaximize.link.next = nullptr;
    m_requestFullscreen.link.next = nullptr;
    m_setTitle.link.next = nullptr;
    m_setAppId.link.next = nullptr;
    m_sceneTree->node.data = nullptr;
    m_toplevel->base->data = nullptr;
    m_server->removeView(this);
  }

  void View::handleRequestMove() {
    m_server->cursor()->beginInteractive(this, m_tiled ? CursorMode::MoveTile : CursorMode::Move, 0);
  }

  void View::handleRequestResize(void* data) {
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    m_server->cursor()->beginInteractive(this, m_tiled ? CursorMode::ResizeTile : CursorMode::Resize, event->edges);
  }

  void View::handleRequestMaximize() {
    if (!m_toplevel->base->initialized) {
      return;
    }

    if (m_tiled && m_workspace != nullptr) {
      const int column = m_workspace->layout().columnOf(this);
      const bool maximized = m_workspace->layout().toggleFullWidth(column);
      wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
      m_workspace->ensureFocusedVisible();
      m_workspace->arrange(false);
      updateForeignState();
      return;
    }

    const bool maximized = !m_toplevel->current.maximized;
    if (maximized) {
      wlr_box usable = m_server->usableAreaAt(m_server->cursor()->wlr()->x, m_server->cursor()->wlr()->y);
      if (usable.width > 0 && usable.height > 0) {
        wlr_xdg_toplevel_set_size(m_toplevel, usable.width, usable.height);
        wlr_scene_node_set_position(&m_sceneTree->node, usable.x, usable.y);
      }
    }
    wlr_xdg_toplevel_set_maximized(m_toplevel, maximized);
    updateForeignState();
  }

  void View::handleRequestFullscreen() {
    if (!m_toplevel->base->initialized) {
      return;
    }

    // Honor the client's requested state (not a blind toggle).
    setFullscreen(m_toplevel->requested.fullscreen);
  }

  void View::toggleFullscreen() {
    if (!m_toplevel->base->initialized) {
      return;
    }
    setFullscreen(!m_toplevel->scheduled.fullscreen);
  }

  void View::toggleFloating() { setFloating(m_tiled); }

  void View::setFloating(bool floating) {
    if (!m_mapped || !m_toplevel->base->initialized) {
      return;
    }
    cancelSizeAnimation();
    // Fullscreen owns its own scene tree; leave it before changing tile/float.
    if (m_toplevel->scheduled.fullscreen || m_toplevel->current.fullscreen) {
      setFullscreen(false);
    }

    const bool wantTiled = !floating;
    if (m_tiled == wantTiled) {
      return;
    }

    if (floating) {
      // Prefer the last acked/scheduled configure size (what the user already sees),
      // not the raw layout target (may be unclamped) or surface geometry (may lag).
      int keepWidth = m_toplevel->current.width;
      int keepHeight = m_toplevel->current.height;
      if (keepWidth <= 0 || keepHeight <= 0) {
        keepWidth = m_toplevel->scheduled.width;
        keepHeight = m_toplevel->scheduled.height;
      }
      if (m_workspace != nullptr) {
        const wlr_box target = m_workspace->layout().targetBox(this);
        if (target.width > 0 && target.height > 0) {
          const XdgSizeHints hints = xdgSizeHints(m_toplevel);
          const int tw = clampXdgWidth(target.width, hints);
          const int th = clampXdgHeight(target.height, hints);
          if (keepWidth <= 0 || keepHeight <= 0) {
            keepWidth = tw;
            keepHeight = th;
          } else {
            keepWidth = std::min(keepWidth, tw);
            keepHeight = std::min(keepHeight, th);
          }
        }
        const int column = m_workspace->layout().columnOf(this);
        if (column >= 0 && m_workspace->layout().isFullWidth(column)) {
          m_workspace->layout().clearFullWidthState(column);
          wlr_xdg_toplevel_set_maximized(m_toplevel, false);
        }
        m_workspace->layoutDetach(this);
      }
      if (keepWidth <= 0 || keepHeight <= 0) {
        const wlr_box& geo = m_toplevel->base->geometry;
        keepWidth = geo.width;
        keepHeight = geo.height;
      }
      const int keepX = m_sceneTree->node.x;
      const int keepY = m_sceneTree->node.y;
      m_tiled = false;
      // Do not clear xdg tiled edges: GTK/Qt often resize (CSD / preferred size) when
      // tiled state is dropped. Floating is a compositor layout concern.
      if (keepWidth > 0
          && keepHeight > 0
          && (m_toplevel->scheduled.width != keepWidth || m_toplevel->scheduled.height != keepHeight)) {
        wlr_xdg_toplevel_set_size(m_toplevel, keepWidth, keepHeight);
      }
      // Nudge down-right so the float is obviously detached from the tile strip.
      wlr_box usable{};
      if (m_workspace != nullptr && m_workspace->group() != nullptr && m_workspace->group()->output() != nullptr) {
        usable = m_workspace->group()->output()->usableArea();
      } else {
        usable = m_server->usableAreaAt(keepX, keepY);
      }
      const int nudge = std::max(32, config().layout.gap * 2);
      int floatX = keepX + nudge;
      int floatY = keepY + nudge;
      if (usable.width > 0 && usable.height > 0 && keepWidth > 0 && keepHeight > 0) {
        floatX = std::clamp(floatX, usable.x, std::max(usable.x, usable.x + usable.width - keepWidth));
        floatY = std::clamp(floatY, usable.y, std::max(usable.y, usable.y + usable.height - keepHeight));
      }
      setPosition(keepX, keepY);
      animateTo(floatX, floatY);
      syncFloatingSurfaceClip();
      // Keep the focus ring when floating a tiled window.
      ensureBorders();
      wlr_scene_node_set_enabled(&m_borderTree->node, true);
      updateBorderGeometry();
      applyCornerRadius();
      updateBlur();
      updateShadow();
      m_server->focusView(this);
      updateForeignState();
      return;
    }

    m_tiled = true;
    wlr_xdg_toplevel_set_tiled(m_toplevel, WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT);
    wlr_xdg_toplevel_set_maximized(m_toplevel, false);
    ensureBorders();
    wlr_scene_node_set_enabled(&m_borderTree->node, true);
    updateBorderGeometry();
    if (m_workspace != nullptr) {
      m_workspace->layoutAttach(this);
    }
    applyCornerRadius();
    updateShadow();
    m_server->focusView(this);
    updateForeignState();
  }

  void View::setFullscreen(bool fullscreen) {
    // Leaving column maximize when entering real fullscreen avoids a stale
    // widthFrac=1.0 column after the client leaves fullscreen.
    if (fullscreen && m_tiled && m_workspace != nullptr) {
      const int column = m_workspace->layout().columnOf(this);
      if (m_workspace->layout().isFullWidth(column)) {
        m_workspace->layout().clearFullWidthState(column);
        wlr_xdg_toplevel_set_maximized(m_toplevel, false);
      }
    }
    wlr_xdg_toplevel_set_fullscreen(m_toplevel, fullscreen);
    cancelSizeAnimation();
    if (fullscreen) {
      // scheduled.fullscreen is set; reparent to fullscreen layer.
      wlr_scene_node_reparent(&m_sceneTree->node, homeTree());
      wlr_scene_node_raise_to_top(&m_sceneTree->node);
      // Snap scroll to the now viewport-wide column and reflow neighbors.
      if (m_workspace != nullptr) {
        m_workspace->ensureFocusedVisible();
        m_workspace->arrange(false);
      } else {
        applyFullscreenLayout();
      }
    } else {
      wlr_scene_node_reparent(&m_sceneTree->node, homeTree());
    }
    if (m_borderTree != nullptr) {
      wlr_scene_node_set_enabled(&m_borderTree->node, !fullscreen);
    }
    applyCornerRadius();
    updateShadow();
    if (!fullscreen) {
      // scheduled.fullscreen is already false; arrange into usable area (exclusive zones).
      if (m_tiled && m_workspace != nullptr) {
        m_workspace->arrange(false);
      } else {
        placeInUsableArea();
      }
    }
    updateForeignState();
  }

} // namespace umbriel
