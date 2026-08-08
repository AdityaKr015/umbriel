#include "overview/overview.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/drop_target.h"
#include "layout/insert_hint.h"
#include "layout/layout.h"
#include "output/output.h"
#include "scene/color.h"
#include "server/server.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <ctime>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("overview");

    // Gap between workspace thumbnails, as a fraction of the scaled row height
    // (niri's Monitor::workspace_gap).
    constexpr double kRowGapFraction = 0.1;
    // Pointer travel that promotes a press on a card into a relocate drag.
    constexpr double kDragThreshold = 10.0;

    int expandedRadius(int radius, int thickness) { return radius > 0 ? radius + thickness : 0; }

    bool boxContains(const wlr_box& box, double x, double y) {
      return x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
    }

    // wlr_scene_rect colors are premultiplied; straight alpha renders as an
    // over-bright wash (scene/color.h).
    std::array<float, 4> tint(const std::array<float, 4>& base, double opacity) {
      std::array<float, 4> out{};
      premultiplied(out.data(), base, static_cast<float>(opacity));
      return out;
    }

    // Cards are pure output: hit testing runs off Overview's own boxes, so scene
    // input must never land on them (Server::viewAt then sees layer surfaces only).
    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }
  } // namespace

  Overview::Overview(Server& server) : m_server(&server) {}

  Overview::~Overview() {
    if (m_anim != 0) {
      m_server->animator().cancel(m_anim);
      m_anim = 0;
    }
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        destroyCard(card.get());
      }
    }
    m_outputs.clear();
  }

  double Overview::zoom() const {
    const double configured = std::clamp(config().overview.zoom, 0.1, 0.75);
    return 1.0 - m_progress * (1.0 - configured);
  }

  // ---------------------------------------------------------------- geometry

  bool Overview::rowMetrics(const OutputState& state, const Server& server, double zoom, RowMetrics& out) {
    wlr_box outputBox{};
    wlr_output_layout_get_box(server.outputLayout(), state.output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }
    out.outputBox = outputBox;
    out.zoom = zoom;
    out.rowW = std::max(1, static_cast<int>(std::lround(outputBox.width * zoom)));
    out.rowH = std::max(1, static_cast<int>(std::lround(outputBox.height * zoom)));
    out.rowX = outputBox.x + static_cast<int>(std::lround((outputBox.width - out.rowW) / 2.0));
    out.baseY = outputBox.y + (outputBox.height - out.rowH) / 2.0;
    out.gap = kRowGapFraction * outputBox.height * zoom;
    return true;
  }

  int Overview::rowTop(const RowMetrics& metrics, double rowScroll, size_t row) {
    const double offset = (static_cast<double>(row) - rowScroll) * (metrics.rowH + metrics.gap);
    return static_cast<int>(std::lround(metrics.baseY + offset));
  }

  wlr_box Overview::worldBoxOf(const View* view, const wlr_box& outputBox) const {
    if (view->toplevel()->scheduled.fullscreen) {
      return outputBox;
    }
    if (const Workspace* workspace = view->workspace()) {
      if (workspace->layout().columnOf(view) >= 0) {
        const wlr_box target = workspace->layout().targetBox(view);
        if (target.width > 0 && target.height > 0) {
          return target;
        }
      }
    }
    const wlr_box& geometry = view->toplevel()->base->geometry;
    return {view->sceneTree()->node.x, view->sceneTree()->node.y, geometry.width, geometry.height};
  }

  void Overview::layoutCard(Card& card, const RowMetrics& metrics, double rowScroll) {
    View* view = card.view;
    const wlr_box& geometry = view->toplevel()->base->geometry;
    if (geometry.width <= 0 || geometry.height <= 0) {
      wlr_scene_node_set_enabled(&card.tree->node, false);
      return;
    }
    wlr_scene_node_set_enabled(&card.tree->node, true);

    const double z = metrics.zoom;
    const int contentW = std::max(1, static_cast<int>(std::lround(geometry.width * z)));
    const int contentH = std::max(1, static_cast<int>(std::lround(geometry.height * z)));
    if (&card == m_dragCard) {
      // The drag owns the card origin; only the scale still tracks progress.
      card.box.width = contentW;
      card.box.height = contentH;
    } else {
      const wlr_box world = worldBoxOf(view, metrics.outputBox);
      card.box = {
          .x = metrics.rowX + static_cast<int>(std::lround((world.x - metrics.outputBox.x) * z)),
          .y =
              rowTop(metrics, rowScroll, card.row) + static_cast<int>(std::lround((world.y - metrics.outputBox.y) * z)),
          .width = contentW,
          .height = contentH,
      };
    }
    wlr_scene_node_set_position(&card.tree->node, card.box.x, card.box.y);

    // Rows overhang the output box by design (adjacent workspaces peek in), so
    // every piece is clipped to the owning output; on a stacked multi-head
    // layout the overhang would otherwise bleed onto the neighbour.
    const bool clipped = &card != m_dragCard;
    wlr_box visible = card.box;
    if (clipped && !wlr_box_intersection(&visible, &card.box, &metrics.outputBox)) {
      wlr_scene_node_set_enabled(&card.tree->node, false);
      return;
    }

    const int total = config().appearance.totalBorderWidth();
    const bool decorated = total > 0 && !view->toplevel()->scheduled.fullscreen;
    const int radius = decorated ? static_cast<int>(std::lround(config().appearance.cornerRadius * z)) : 0;
    if (card.border != nullptr) {
      const int width = std::max(1, static_cast<int>(std::lround(total * z)));
      const wlr_box full{card.box.x - width, card.box.y - width, contentW + 2 * width, contentH + 2 * width};
      wlr_box border = full;
      const bool borderVisible = decorated && (!clipped || wlr_box_intersection(&border, &full, &metrics.outputBox));
      wlr_scene_node_set_enabled(&card.border->node, borderVisible);
      if (borderVisible) {
        wlr_scene_rect_set_size(card.border, border.width, border.height);
        wlr_scene_node_set_position(&card.border->node, border.x - card.box.x, border.y - card.box.y);
        wlr_scene_rect_set_corner_radius(
            card.border, static_cast<int>(std::lround(expandedRadius(config().appearance.cornerRadius, total) * z))
        );
        // Punch the content out so this stays a ring. A filled rect would sit
        // behind the window and tint every translucent client with the border
        // colour, which then visibly changes with focus.
        const wlr_box hole{card.box.x - border.x, card.box.y - border.y, contentW, contentH};
        wlr_scene_rect_set_clipped_region(
            card.border, clipped_region{.area = hole, .corners = corner_radii_new(radius, radius, radius, radius)}
        );
        // Every row advertises its own focused window, not just the active
        // one: the filmstrip is a browsing aid, and the border is what tells
        // you where each workspace will land you when you zoom into it.
        const Workspace* workspace = view->workspace();
        const bool focused = workspace != nullptr && workspace->focusedView() == view && m_dragCard != &card;
        const std::array<float, 4> color =
            tint(focused ? config().appearance.borderFocused : config().appearance.borderUnfocused, 1.0);
        wlr_scene_rect_set_color(card.border, color.data());
      }
    }

    const double fx = static_cast<double>(contentW) / geometry.width;
    const double fy = static_cast<double>(contentH) / geometry.height;
    for (const auto& entry : card.surfaces) {
      wlr_surface* surface = entry->surface;
      if (entry->buffer == nullptr || surface->current.width <= 0 || surface->current.height <= 0) {
        continue;
      }
      if (!entry->isRoot) {
        wlr_box sub{
            card.box.x + static_cast<int>(std::lround((entry->sx - geometry.x) * fx)),
            card.box.y + static_cast<int>(std::lround((entry->sy - geometry.y) * fy)),
            std::max(1, static_cast<int>(std::lround(surface->current.width * fx))),
            std::max(1, static_cast<int>(std::lround(surface->current.height * fy))),
        };
        wlr_box subVisible{};
        if (clipped && !wlr_box_intersection(&subVisible, &sub, &metrics.outputBox)) {
          wlr_scene_node_set_enabled(&entry->buffer->node, false);
          continue;
        }
        wlr_scene_node_set_enabled(&entry->buffer->node, true);
        wlr_scene_node_set_position(&entry->buffer->node, sub.x - card.box.x, sub.y - card.box.y);
        wlr_scene_buffer_set_dest_size(entry->buffer, sub.width, sub.height);
        continue;
      }
      // Root surface: crop to the committed window geometry so CSD shadow
      // padding never leaks into the thumbnail, then scale the visible part of
      // that region onto the card.
      wlr_fbox base{};
      wlr_surface_get_buffer_source_box(surface, &base);
      const double bx = base.width / surface->current.width;
      const double by = base.height / surface->current.height;
      // Surface-local region backing `visible`.
      const double sx = geometry.x + (visible.x - card.box.x) / fx;
      const double sy = geometry.y + (visible.y - card.box.y) / fy;
      wlr_fbox src{base.x + sx * bx, base.y + sy * by, (visible.width / fx) * bx, (visible.height / fy) * by};
      if (src.x < base.x) {
        src.width -= base.x - src.x;
        src.x = base.x;
      }
      if (src.y < base.y) {
        src.height -= base.y - src.y;
        src.y = base.y;
      }
      src.width = std::min(src.width, base.x + base.width - src.x);
      src.height = std::min(src.height, base.y + base.height - src.y);
      if (src.width <= 0 || src.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      wlr_scene_node_set_enabled(&entry->buffer->node, true);
      wlr_scene_node_set_position(&entry->buffer->node, visible.x - card.box.x, visible.y - card.box.y);
      wlr_scene_buffer_set_source_box(entry->buffer, &src);
      wlr_scene_buffer_set_dest_size(entry->buffer, visible.width, visible.height);
      wlr_scene_buffer_set_corner_radius(entry->buffer, radius);
    }
  }

  void Overview::layoutOutput(OutputState& state) {
    RowMetrics metrics{};
    if (!rowMetrics(state, *m_server, zoom(), metrics)) {
      return;
    }

    wlr_scene_node_set_position(&state.backdrop->node, metrics.outputBox.x, metrics.outputBox.y);
    wlr_scene_rect_set_size(state.backdrop, metrics.outputBox.width, metrics.outputBox.height);
    const std::array<float, 4> backdrop = tint(config().overview.backdropColor, m_progress);
    // A fully transparent backdrop means "leave the wallpaper alone"; skip the
    // full-screen draw entirely. Input is gated in Cursor, not by this rect.
    wlr_scene_node_set_enabled(&state.backdrop->node, backdrop[3] > 0.001F);
    wlr_scene_rect_set_color(state.backdrop, backdrop.data());

    const std::array<float, 4> rowColor = tint(config().overview.workspaceColor, m_progress);
    for (size_t row = 0; row < state.rowRects.size(); ++row) {
      wlr_scene_rect* rect = state.rowRects[row];
      const wlr_box full{metrics.rowX, rowTop(metrics, state.rowScroll, row), metrics.rowW, metrics.rowH};
      wlr_box visible{};
      if (!wlr_box_intersection(&visible, &full, &metrics.outputBox)) {
        wlr_scene_node_set_enabled(&rect->node, false);
        continue;
      }
      wlr_scene_node_set_enabled(&rect->node, true);
      wlr_scene_node_set_position(&rect->node, visible.x, visible.y);
      wlr_scene_rect_set_size(rect, visible.width, visible.height);
      wlr_scene_rect_set_color(rect, rowColor.data());
      wlr_scene_rect_set_corner_radius(rect, config().appearance.cornerRadius);
    }

    for (const auto& card : state.cards) {
      layoutCard(*card, metrics, state.rowScroll);
    }
  }

  void Overview::applyProgress() {
    for (const auto& state : m_outputs) {
      layoutOutput(*state);
    }
    scheduleFrames();
  }

  void Overview::scheduleFrames() const {
    for (const auto& state : m_outputs) {
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  // ------------------------------------------------------------------- cards

  void Overview::addCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    auto* card = static_cast<Card*>(data);
    wlr_scene_surface* sceneSurface = wlr_scene_surface_create(card->tree, surface);
    if (sceneSurface == nullptr) {
      return;
    }
    auto entry = std::make_unique<CardSurface>();
    entry->card = card;
    entry->surface = surface;
    entry->buffer = sceneSurface->buffer;
    entry->sx = sx;
    entry->sy = sy;
    entry->isRoot = surface == card->view->toplevel()->base->surface;
    wlr_scene_buffer_set_filter_mode(entry->buffer, WLR_SCALE_FILTER_BILINEAR);
    entry->buffer->point_accepts_input = rejectInput;
    entry->commit.notify = onCardSurfaceCommit;
    wl_signal_add(&surface->events.commit, &entry->commit);
    entry->destroy.notify = onCardSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &entry->destroy);
    card->surfaces.push_back(std::move(entry));
  }

  void Overview::syncCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    auto* card = static_cast<Card*>(data);
    for (const auto& entry : card->surfaces) {
      if (entry->surface == surface) {
        entry->sx = sx;
        entry->sy = sy;
        return;
      }
    }
    addCardSurface(surface, sx, sy, data);
  }

  void Overview::onCardSurfaceCommit(wl_listener* listener, void* /*data*/) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, commit);
    Card* card = entry->card;
    Overview* self = card->overview;
    if (!self->m_active || card->owner == nullptr) {
      return;
    }
    // The scene surface reconfigures on every commit, resetting dest size and
    // source box; re-derive both (and pick up new/moved subsurfaces).
    wlr_surface_for_each_surface(card->view->toplevel()->base->surface, syncCardSurface, card);
    RowMetrics metrics{};
    if (rowMetrics(*card->owner, *self->m_server, self->zoom(), metrics)) {
      self->layoutCard(*card, metrics, card->owner->rowScroll);
      wlr_output_schedule_frame(card->owner->output->wlr());
    }
  }

  void Overview::onCardSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, destroy);
    Card* card = entry->card;
    wl_list_remove(&entry->commit.link);
    wl_list_remove(&entry->destroy.link);
    std::erase_if(card->surfaces, [entry](const std::unique_ptr<CardSurface>& candidate) {
      return candidate.get() == entry;
    });
  }

  Overview::Card* Overview::createCard(OutputState& state, View* view, size_t row) {
    wlr_surface* surface = view->toplevel()->base->surface;
    if (surface == nullptr) {
      return nullptr;
    }
    auto card = std::make_unique<Card>();
    card->overview = this;
    card->owner = &state;
    card->view = view;
    card->row = row;
    card->tree = wlr_scene_tree_create(state.tree);
    if (card->tree == nullptr) {
      return nullptr;
    }
    const std::array<float, 4> borderColor = tint(config().appearance.borderUnfocused, 1.0);
    card->border = wlr_scene_rect_create(card->tree, 1, 1, borderColor.data());
    Card* raw = card.get();
    state.cards.push_back(std::move(card));

    wlr_surface_for_each_surface(surface, addCardSurface, raw);

    // Commit-throttled clients only render when a frame callback fires. The
    // real tree just went invisible, so feed one frame to restart their loop;
    // the card's own scene surfaces drive it from there.
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (const auto& entry : raw->surfaces) {
      wlr_surface_send_frame_done(entry->surface, &now);
    }
    return raw;
  }

  void Overview::destroyCard(Card* card) {
    for (const auto& entry : card->surfaces) {
      wl_list_remove(&entry->commit.link);
      wl_list_remove(&entry->destroy.link);
    }
    card->surfaces.clear();
    if (card->tree != nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      card->tree = nullptr;
    }
    card->border = nullptr;
  }

  void Overview::dropCard(View* view) {
    for (const auto& state : m_outputs) {
      const auto it =
          std::ranges::find_if(state->cards, [view](const std::unique_ptr<Card>& card) { return card->view == view; });
      if (it == state->cards.end()) {
        continue;
      }
      destroyCard(it->get());
      state->cards.erase(it);
      return;
    }
  }

  void Overview::rebuildCard(View* view) {
    dropCard(view);
    Workspace* workspace = view->workspace();
    if (workspace == nullptr || !view->mapped()) {
      return;
    }
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr) {
      return;
    }
    createCard(*state, view, workspace->index());
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
  }

  Overview::OutputState* Overview::stateFor(const Output* output) {
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    return it == m_outputs.end() ? nullptr : it->get();
  }

  Overview::OutputState* Overview::stateForWorkspace(const Workspace* workspace) {
    if (workspace == nullptr || workspace->group() == nullptr) {
      return nullptr;
    }
    return stateFor(workspace->group()->output());
  }

  Overview::Card* Overview::findCard(const View* view) {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        if (card->view == view) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  void Overview::populateCards(OutputState& state) {
    WorkspaceGroup* group = state.output->workspaceGroup();
    if (group == nullptr) {
      return;
    }
    for (size_t row = 0; row < group->workspaceCount(); ++row) {
      Workspace* workspace = group->workspaceAt(row);
      if (workspace == nullptr) {
        continue;
      }
      // Tiled, then floating, then fullscreen: mirrors the per-workspace scene
      // layer split so overlapping cards stack the way the real windows do.
      for (int pass = 0; pass < 3; ++pass) {
        for (View* view : workspace->allViews()) {
          if (view == nullptr || !view->mapped()) {
            continue;
          }
          const bool fullscreen = view->toplevel()->scheduled.fullscreen;
          const int layer = fullscreen ? 2 : (view->tiled() ? 0 : 1);
          if (layer == pass) {
            createCard(state, view, row);
          }
        }
      }
    }
  }

  void Overview::buildState() {
    m_tree = m_server->overviewTree();
    for (const auto& output : m_server->m_outputs) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr || group->workspaceCount() == 0) {
        continue;
      }
      auto state = std::make_unique<OutputState>();
      state->output = output.get();
      state->tree = wlr_scene_tree_create(m_tree);
      if (state->tree == nullptr) {
        continue;
      }
      const std::array<float, 4> backdropColor = tint(config().overview.backdropColor, 0.0);
      state->backdrop = wlr_scene_rect_create(state->tree, 1, 1, backdropColor.data());
      wlr_scene_rect_set_corner_radius(state->backdrop, 0);
      state->rowRects.reserve(group->workspaceCount());
      const std::array<float, 4> rowColor = tint(config().overview.workspaceColor, 0.0);
      for (size_t row = 0; row < group->workspaceCount(); ++row) {
        state->rowRects.push_back(wlr_scene_rect_create(state->tree, 1, 1, rowColor.data()));
      }
      state->rowScroll = group->active() != nullptr ? static_cast<double>(group->active()->index()) : 0.0;
      state->rowFrom = state->rowScroll;
      state->rowTo = state->rowScroll;
      OutputState* raw = state.get();
      m_outputs.push_back(std::move(state));
      populateCards(*raw);
    }
  }

  // -------------------------------------------------------------- open/close

  bool Overview::beginPresentation() {
    if (m_active) {
      return true;
    }
    if (m_server->sessionLocked()) {
      return false;
    }
    m_server->cursor()->resetMode();
    for (const auto& output : m_server->m_outputs) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      group->slideFinish();
      // Settle the visual scroll onto the layout scroll so cards at progress 0
      // sit exactly where the real windows are.
      if (Workspace* workspace = group->active()) {
        workspace->arrange(false);
      }
    }

    buildState();
    if (m_outputs.empty()) {
      return false;
    }

    m_active = true;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pendingFocus = nullptr;
    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, false);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, false);
    wlr_scene_node_set_enabled(&m_tree->node, true);

    m_server->clearKeyboardFocus();
    wlr_seat_pointer_clear_focus(m_server->seat()->wlr());
    m_server->cursor()->clearConstraint();

    applyProgress();
    for (const auto& state : m_outputs) {
      state->output->markBlurBackgroundDirty();
    }
    kLog.debug("overview opened on {} output(s)", m_outputs.size());
    return true;
  }

  void Overview::open() {
    if (m_active) {
      m_closing = false;
      m_pendingFocus = nullptr;
      if (m_progress < 1.0 || m_targetProgress < 1.0) {
        startAnimation(1.0, false);
      }
      return;
    }
    if (!beginPresentation()) {
      return;
    }
    startAnimation(1.0, false);
  }

  void Overview::toggle() {
    if (m_active && !m_closing) {
      close();
    } else {
      open();
    }
  }

  void Overview::close() { beginClose(nullptr); }

  void Overview::closeToWorkspace(Workspace* workspace, View* focus) {
    if (!m_active || m_closing) {
      return;
    }
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->active() != workspace) {
      // No slide: the real trees are hidden, the filmstrip is the transition.
      workspace->group()->activate(workspace, false);
    }
    beginClose(focus);
  }

  void Overview::beginClose(View* focus) {
    if (!m_active || m_closing) {
      return;
    }
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    hideDropHint();
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_pendingFocus = focus;
    for (const auto& state : m_outputs) {
      WorkspaceGroup* group = state->output->workspaceGroup();
      state->rowFrom = state->rowScroll;
      state->rowTo = (group != nullptr && group->active() != nullptr) ? static_cast<double>(group->active()->index())
                                                                      : state->rowScroll;
    }
    startAnimation(0.0, true);
  }

  void Overview::forceClose() {
    if (!m_active) {
      return;
    }
    if (m_anim != 0) {
      m_server->animator().cancel(m_anim);
      m_anim = 0;
    }
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    m_pendingFocus = nullptr;
    teardown();
    m_server->refocus();
  }

  void Overview::startAnimation(double target, bool closing) {
    if (m_anim != 0) {
      m_server->animator().cancel(m_anim);
      m_anim = 0;
    }
    m_closing = closing;
    m_targetProgress = target;
    const double start = m_progress;
    m_anim = m_server->animator().animate(
        0.0, 1.0, std::max(1, config().appearance.animationMs), Easing::EaseOutCubic,
        [this, start, target](double value) {
          m_progress = start + (target - start) * value;
          for (const auto& state : m_outputs) {
            state->rowScroll = state->rowFrom + (state->rowTo - state->rowFrom) * value;
          }
          applyProgress();
        },
        [this] {
          m_anim = 0;
          finishAnimation();
        }
    );
    // The animator only ticks from an output frame, and it clocks from
    // animate(). Without a frame pending (idle desktop, e.g. closing after the
    // open animation settled) the first tick would arrive past the duration and
    // snap straight to the target; kick one frame so it self-sustains.
    scheduleFrames();
  }

  void Overview::finishAnimation() {
    if (!m_closing) {
      applyProgress();
      return;
    }
    View* focus = m_pendingFocus;
    m_pendingFocus = nullptr;
    teardown();
    // Real trees are visible again: settle each active workspace so window
    // positions match where the cards landed.
    for (const auto& output : m_server->m_outputs) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        if (Workspace* workspace = group->active()) {
          workspace->arrange(false);
        }
      }
    }
    if (focus != nullptr && focus->mapped()) {
      m_server->focusView(focus, FocusReason::PointerPress);
    } else {
      m_server->refocus();
    }
  }

  void Overview::teardown() {
    hideDropHint();
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        destroyCard(card.get());
      }
      state->cards.clear();
      if (state->tree != nullptr) {
        wlr_scene_node_destroy(&state->tree->node);
        state->tree = nullptr;
      }
      state->output->markBlurBackgroundDirty();
      wlr_output_schedule_frame(state->output->wlr());
    }
    m_outputs.clear();

    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, true);

    m_active = false;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    m_dragCard = nullptr;
    m_dragSourceWorkspace = nullptr;
    m_dropWorkspace = nullptr;
    m_dropColumn = -1;
    m_dropRow = -1;
    m_gestureOpenedHere = false;
  }

  // ----------------------------------------------------------------- gesture

  void Overview::gestureUpdate(double progress) {
    if (!m_active) {
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedHere = true;
    }
    if (m_anim != 0) {
      m_server->animator().cancel(m_anim);
      m_anim = 0;
    }
    m_closing = false;
    m_progress = progress;
    m_targetProgress = progress;
    applyProgress();
  }

  void Overview::gestureEnd(bool commitOpen) {
    if (!m_active) {
      return;
    }
    m_gestureOpenedHere = false;
    if (commitOpen) {
      startAnimation(1.0, false);
      return;
    }
    beginClose(nullptr);
  }

  // ------------------------------------------------------------------- hooks

  void Overview::onViewMapped(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr || findCard(view) != nullptr) {
      return;
    }
    createCard(*state, view, workspace->index());
    layoutOutput(*state);
    wlr_output_schedule_frame(state->output->wlr());
  }

  void Overview::onViewUnmapped(View* view) {
    if (!m_active || view == nullptr) {
      return;
    }
    if (m_pendingFocus == view) {
      m_pendingFocus = nullptr;
    }
    if (m_pressCard != nullptr && m_pressCard->view == view) {
      m_pressCard = nullptr;
    }
    if (m_dragCard != nullptr && m_dragCard->view == view) {
      hideDropHint();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_dropWorkspace = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
    OutputState* state = stateForWorkspace(view->workspace());
    dropCard(view);
    if (state != nullptr) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onWorkspaceActivated(WorkspaceGroup* group) {
    if (!m_active || m_closing || group == nullptr || group->active() == nullptr) {
      return;
    }
    OutputState* target = stateFor(group->output());
    if (target == nullptr) {
      return;
    }
    const auto row = static_cast<double>(group->active()->index());
    if (std::abs(target->rowTo - row) < 0.001 && m_anim != 0) {
      return;
    }
    for (const auto& state : m_outputs) {
      state->rowFrom = state->rowScroll;
      state->rowTo = state->rowScroll;
    }
    target->rowTo = row;
    startAnimation(m_targetProgress, false);
  }

  void Overview::onWorkspaceArranged(Workspace* workspace) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateForWorkspace(workspace)) {
      layoutOutput(*state);
      wlr_output_schedule_frame(state->output->wlr());
    }
  }

  void Overview::onFocusChanged() {
    if (m_active) {
      applyProgress();
    }
  }

  void Overview::onOutputRemoved(Output* output) {
    if (!m_active) {
      return;
    }
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    if (it == m_outputs.end()) {
      return;
    }
    if (m_dragCard != nullptr && m_dragCard->owner == it->get()) {
      hideDropHint();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_dropWorkspace = nullptr;
      m_server->cursor()->overrideCursor(nullptr);
    }
    if (m_pressCard != nullptr && m_pressCard->owner == it->get()) {
      m_pressCard = nullptr;
    }
    for (const auto& card : (*it)->cards) {
      destroyCard(card.get());
    }
    (*it)->cards.clear();
    if ((*it)->tree != nullptr) {
      wlr_scene_node_destroy(&(*it)->tree->node);
    }
    m_outputs.erase(it);
    if (m_outputs.empty()) {
      forceClose();
    }
  }

  // ------------------------------------------------------------- hit testing

  Overview::Card* Overview::cardAt(double lx, double ly) {
    // Topmost first: later outputs and later cards paint over earlier ones.
    for (const auto& state : std::views::reverse(m_outputs)) {
      for (const auto& card : std::views::reverse(state->cards)) {
        if (card->tree != nullptr && card->tree->node.enabled && boxContains(card->box, lx, ly)) {
          return card.get();
        }
      }
    }
    return nullptr;
  }

  Workspace* Overview::rowAt(double lx, double ly, OutputState** outState, size_t* outRow) {
    for (const auto& state : m_outputs) {
      RowMetrics metrics{};
      if (!rowMetrics(*state, *m_server, zoom(), metrics)) {
        continue;
      }
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      for (size_t row = 0; row < state->rowRects.size(); ++row) {
        const wlr_box box{metrics.rowX, rowTop(metrics, state->rowScroll, row), metrics.rowW, metrics.rowH};
        if (!boxContains(box, lx, ly)) {
          continue;
        }
        Workspace* workspace = group->workspaceAt(row);
        if (workspace == nullptr) {
          continue;
        }
        if (outState != nullptr) {
          *outState = state.get();
        }
        if (outRow != nullptr) {
          *outRow = row;
        }
        return workspace;
      }
    }
    return nullptr;
  }

  Workspace* Overview::preferredWorkspace() const {
    Output* output = m_server->outputFromWlr(m_server->preferredOutput());
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return nullptr;
    }
    return output->workspaceGroup()->active();
  }

  // ------------------------------------------------------------------- input

  bool Overview::handleButton(uint32_t button, bool pressed, double lx, double ly) {
    if (!interactive()) {
      return true; // Swallow everything while zooming back in.
    }

    if (!pressed) {
      if (button != BTN_LEFT) {
        return true;
      }
      if (m_dragCard != nullptr) {
        endDrag(true);
        return true;
      }
      Card* card = m_pressCard;
      Workspace* workspace = m_pressWorkspace;
      m_pressCard = nullptr;
      m_pressWorkspace = nullptr;
      if (card != nullptr && card->view != nullptr && card->view->mapped()) {
        closeToWorkspace(card->view->workspace(), card->view);
      } else if (workspace != nullptr) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    }

    Card* card = cardAt(lx, ly);
    if (button == BTN_MIDDLE) {
      if (card != nullptr && card->view != nullptr) {
        wlr_xdg_toplevel_send_close(card->view->toplevel());
      }
      return true;
    }
    if (button != BTN_LEFT) {
      return true;
    }

    m_pressCard = card;
    m_pressWorkspace = nullptr;
    m_pressX = lx;
    m_pressY = ly;
    if (card == nullptr) {
      m_pressWorkspace = rowAt(lx, ly, nullptr, nullptr);
    }
    return true;
  }

  void Overview::handleMotion(double lx, double ly) {
    if (!interactive()) {
      return;
    }
    if (m_dragCard != nullptr) {
      updateDrag(lx, ly);
      return;
    }
    if (m_pressCard == nullptr) {
      return;
    }
    const double dx = lx - m_pressX;
    const double dy = ly - m_pressY;
    if (dx * dx + dy * dy < kDragThreshold * kDragThreshold) {
      return;
    }
    beginDrag();
    updateDrag(lx, ly);
  }

  bool Overview::handleAxisNotch(bool vertical, double direction, double lx, double ly) {
    if (!interactive() || !vertical) {
      return true;
    }
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), lx, ly);
    Output* output = m_server->outputFromWlr(wlrOutput);
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return true;
    }
    WorkspaceGroup* group = output->workspaceGroup();
    if (group->active() == nullptr) {
      return true;
    }
    const int index = static_cast<int>(group->active()->index()) + (direction < 0 ? -1 : 1);
    if (index < 0 || index >= static_cast<int>(group->workspaceCount())) {
      return true;
    }
    group->select(group->workspaceAt(static_cast<size_t>(index)));
    return true;
  }

  bool Overview::handleFallbackKey(uint32_t keysym) {
    if (!interactive()) {
      return false;
    }
    switch (keysym) {
    case XKB_KEY_Escape:
      close();
      return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      if (Workspace* workspace = preferredWorkspace()) {
        closeToWorkspace(workspace, workspace->focusedView());
      }
      return true;
    case XKB_KEY_Left:
    case XKB_KEY_Right:
      if (Workspace* workspace = preferredWorkspace()) {
        if (View* target = workspace->focusAdjacent(keysym == XKB_KEY_Left ? -1 : 1)) {
          m_server->focusView(target, FocusReason::Directional);
        }
      }
      return true;
    case XKB_KEY_Up:
    case XKB_KEY_Down: {
      Output* output = m_server->outputFromWlr(m_server->preferredOutput());
      if (output == nullptr || output->workspaceGroup() == nullptr || output->workspaceGroup()->active() == nullptr) {
        return true;
      }
      WorkspaceGroup* group = output->workspaceGroup();
      const int index = static_cast<int>(group->active()->index()) + (keysym == XKB_KEY_Up ? -1 : 1);
      if (index >= 0 && index < static_cast<int>(group->workspaceCount())) {
        group->select(group->workspaceAt(static_cast<size_t>(index)));
      }
      return true;
    }
    default:
      return false;
    }
  }

  // -------------------------------------------------------------------- drag

  void Overview::beginDrag() {
    Card* card = m_pressCard;
    m_pressCard = nullptr;
    m_pressWorkspace = nullptr;
    if (card == nullptr || card->view == nullptr || !card->view->mapped()) {
      return;
    }
    View* view = card->view;
    m_dragCard = card;
    m_dragOffsetX = m_pressX - card->box.x;
    m_dragOffsetY = m_pressY - card->box.y;
    m_dragSourceWorkspace = view->workspace();
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dropWorkspace = nullptr;
    m_dropColumn = -1;
    m_dropRow = -1;

    if (m_dragSourceWorkspace != nullptr && view->tiled()) {
      m_dragSourceColumn = m_dragSourceWorkspace->layout().columnOf(view);
      m_dragSourceRow = m_dragSourceWorkspace->layout().rowOf(view);
      if (m_dragSourceColumn >= 0) {
        // Detach so the source row closes the gap live, exactly like a normal
        // tile drag; arrange() re-lays that output's cards through the hook.
        m_dragSourceWorkspace->layoutDetach(view, false);
      }
    }
    wlr_scene_node_reparent(&card->tree->node, m_tree);
    wlr_scene_node_raise_to_top(&card->tree->node);
    m_server->cursor()->overrideCursor("grabbing");
  }

  void Overview::updateDrag(double lx, double ly) {
    Card* card = m_dragCard;
    card->box.x = static_cast<int>(std::lround(lx - m_dragOffsetX));
    card->box.y = static_cast<int>(std::lround(ly - m_dragOffsetY));
    wlr_scene_node_set_position(&card->tree->node, card->box.x, card->box.y);

    OutputState* state = nullptr;
    size_t row = 0;
    Workspace* workspace = rowAt(lx, ly, &state, &row);
    m_dropWorkspace = workspace;
    m_dropColumn = -1;
    m_dropRow = -1;
    if (workspace == nullptr || state == nullptr) {
      hideDropHint();
      scheduleFrames();
      return;
    }

    RowMetrics metrics{};
    if (!rowMetrics(*state, *m_server, zoom(), metrics)) {
      hideDropHint();
      return;
    }
    // Map the pointer out of the thumbnail and back into workspace world space.
    const double worldX = metrics.outputBox.x + (lx - metrics.rowX) / metrics.zoom;
    const double worldY = metrics.outputBox.y + (ly - rowTop(metrics, state->rowScroll, row)) / metrics.zoom;

    if (workspace->layoutMode() != LayoutMode::Scrolling) {
      // Dwindle rows accept append-only drops: aiming at a split edge inside a
      // half-scale thumbnail is guesswork, so drop at the end of the tree.
      m_dropColumn = static_cast<int>(workspace->layout().columns().size());
      hideDropHint();
      scheduleFrames();
      return;
    }

    const wlr_box usable = state->output->usableArea();
    const double scroll = workspace->layout().scroll();
    const ScrollingDropTarget target = computeScrollingDropTarget(*workspace, usable, scroll, worldX, worldY);
    m_dropColumn = target.column;
    m_dropRow = target.row;
    const wlr_box hint = target.row >= 0 ? InsertHint::rowHintBox(*workspace, target.column, target.row, scroll)
                                         : InsertHint::gapHintBox(*workspace, target.column, scroll);
    showDropHint(hint, metrics, state->rowScroll, row);
    scheduleFrames();
  }

  void Overview::endDrag(bool drop) {
    Card* card = m_dragCard;
    if (card == nullptr) {
      return;
    }
    View* view = card->view;
    Workspace* target = drop ? m_dropWorkspace : nullptr;
    const int column = m_dropColumn;
    const int row = m_dropRow;
    const wlr_box cardBox = card->box;
    OutputState* dropState = target != nullptr ? stateForWorkspace(target) : nullptr;

    m_dragCard = nullptr;
    m_dropWorkspace = nullptr;
    m_dropColumn = -1;
    m_dropRow = -1;
    hideDropHint();
    m_server->cursor()->overrideCursor(nullptr);

    if (view == nullptr || !view->mapped()) {
      return;
    }

    if (target != nullptr && view->tiled()) {
      if (view->workspace() != target) {
        // Explicit insertion below; the auto-attach would land it elsewhere.
        view->setWorkspace(target, /*attachToLayout=*/false);
      }
      if (target->layoutMode() != LayoutMode::Scrolling) {
        target->layout().insertView(view, static_cast<int>(target->layout().columns().size()));
      } else if (row >= 0) {
        target->layout().insertViewIntoColumn(view, std::max(0, column), row);
      } else {
        target->layout().insertView(view, std::max(0, column));
      }
      target->arrange(false);
      m_server->focusView(view, FocusReason::DragDrop);
    } else if (target != nullptr && dropState != nullptr) {
      // Floating: map the card origin back out of the thumbnail.
      RowMetrics metrics{};
      size_t targetRow = target->index();
      if (rowMetrics(*dropState, *m_server, zoom(), metrics)) {
        const int x = metrics.outputBox.x + static_cast<int>(std::lround((cardBox.x - metrics.rowX) / metrics.zoom));
        const int y = metrics.outputBox.y
            + static_cast<int>(
                          std::lround((cardBox.y - rowTop(metrics, dropState->rowScroll, targetRow)) / metrics.zoom)
            );
        if (view->workspace() != target) {
          view->setWorkspace(target, /*attachToLayout=*/false);
        }
        view->setPosition(x, y);
      }
    } else if (m_dragSourceWorkspace != nullptr && view->tiled() && m_dragSourceColumn >= 0) {
      // Cancelled or dropped on nothing: put the tile back where it came from.
      if (m_dragSourceRow >= 0) {
        m_dragSourceWorkspace->layout().insertViewIntoColumn(view, m_dragSourceColumn, m_dragSourceRow);
      } else {
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
      }
      m_dragSourceWorkspace->arrange(false);
    }

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCard(view);
    applyProgress();
  }

  void Overview::showDropHint(const wlr_box& worldBox, const RowMetrics& metrics, double rowScroll, size_t row) {
    if (worldBox.width <= 0 || worldBox.height <= 0) {
      hideDropHint();
      return;
    }
    if (m_dropHint == nullptr) {
      m_dropHint = wlr_scene_rect_create(m_tree, 1, 1, config().appearance.insertHintColor.data());
      if (m_dropHint == nullptr) {
        return;
      }
    }
    const double z = metrics.zoom;
    const int x = metrics.rowX + static_cast<int>(std::lround((worldBox.x - metrics.outputBox.x) * z));
    const int y =
        rowTop(metrics, rowScroll, row) + static_cast<int>(std::lround((worldBox.y - metrics.outputBox.y) * z));
    const std::array<float, 4> hintColor = tint(config().appearance.insertHintColor, 1.0);
    wlr_scene_rect_set_color(m_dropHint, hintColor.data());
    wlr_scene_rect_set_size(
        m_dropHint, std::max(1, static_cast<int>(std::lround(worldBox.width * z))),
        std::max(1, static_cast<int>(std::lround(worldBox.height * z)))
    );
    wlr_scene_rect_set_corner_radius(m_dropHint, static_cast<int>(std::lround(config().appearance.cornerRadius * z)));
    wlr_scene_node_set_position(&m_dropHint->node, x, y);
    wlr_scene_node_set_enabled(&m_dropHint->node, true);
    // Below the dragged card, above every thumbnail.
    wlr_scene_node_raise_to_top(&m_dropHint->node);
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
    }
  }

  void Overview::hideDropHint() {
    if (m_dropHint != nullptr) {
      wlr_scene_node_set_enabled(&m_dropHint->node, false);
    }
  }

} // namespace umbriel
