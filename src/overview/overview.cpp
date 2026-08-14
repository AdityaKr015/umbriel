#include "overview/overview.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layout/drop_target.h"
#include "layout/layout.h"
#include "output/output.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/hint_rect.h"
#include "server/server.h"
#include "view/output_clip.h"
#include "view/view.h"
// clang-format off
#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr.h"
// clang-format on
#include "workspace/workspace.h"

namespace umbriel {

  namespace {
    constexpr Logger kLog("overview");

    // Gap between workspace thumbnails, as a fraction of the scaled row height.
    constexpr double kRowGapFraction = 0.1;
    // Pointer travel that promotes a press on a card into a relocate drag.
    constexpr double kDragThreshold = 10.0;

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
    void layoutWorkspaceBackground(
        wlr_scene_rect* background, const wlr_box& full, const wlr_box& clip, int radius,
        const std::array<float, 4>& color
    ) {
      if (background == nullptr) {
        return;
      }
      wlr_box visible{};
      if (color[3] <= 0.001F || !wlr_box_intersection(&visible, &full, &clip)) {
        wlr_scene_node_set_enabled(&background->node, false);
        return;
      }

      wlr_scene_node_set_enabled(&background->node, true);
      wlr_scene_node_set_position(&background->node, visible.x, visible.y);
      wlr_scene_rect_set_size(background, visible.width, visible.height);
      wlr_scene_rect_set_color(background, color.data());
      wlr_scene_rect_set_clipped_region(background, clipped_region_get_default());

      const bool trimLeft = visible.x > full.x;
      const bool trimRight = visible.x + visible.width < full.x + full.width;
      const bool trimTop = visible.y > full.y;
      const bool trimBottom = visible.y + visible.height < full.y + full.height;
      wlr_scene_rect_set_corner_radii(
          background,
          corner_radii_new(
              trimLeft || trimTop ? 0 : radius, trimRight || trimTop ? 0 : radius, trimRight || trimBottom ? 0 : radius,
              trimLeft || trimBottom ? 0 : radius
          )
      );
    }

    // Cards are pure output: hit testing runs off Overview's own boxes, so scene
    // input must never land on them (Server::viewAt then sees layer surfaces only).
    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*sx*/, double* /*sy*/) { return false; }

    wlr_scene_buffer* sourceBufferForSurface(wlr_scene_node* node, wlr_surface* surface) {
      if (node->type == WLR_SCENE_NODE_BUFFER) {
        wlr_scene_buffer* buffer = wlr_scene_buffer_from_node(node);
        wlr_scene_surface* sceneSurface = wlr_scene_surface_try_from_buffer(buffer);
        return sceneSurface != nullptr && sceneSurface->surface == surface ? buffer : nullptr;
      }
      if (node->type != WLR_SCENE_NODE_TREE) {
        return nullptr;
      }
      wlr_scene_tree* tree = wlr_scene_tree_from_node(node);
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &tree->children, link) {
        if (wlr_scene_buffer* buffer = sourceBufferForSurface(child, surface)) {
          return buffer;
        }
      }
      return nullptr;
    }
  } // namespace

  Overview::Overview(Server& server) : m_server(&server) { m_server->registerAnimatable(this); }

  Overview::~Overview() {
    m_server->unregisterAnimatable(this);
    m_anim.snap(0.0);
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
    const wlr_box& geometry = view->toplevel()->base->geometry;
    if (view->toplevel()->current.fullscreen && geometry.width > 0 && geometry.height > 0) {
      // Match View::updateFullscreenPresentation: committed fullscreen content
      // is centered without scaling, and oversized buffers are cropped evenly.
      return {
          outputBox.x + (outputBox.width - geometry.width) / 2,
          outputBox.y + (outputBox.height - geometry.height) / 2,
          geometry.width,
          geometry.height,
      };
    }
    if (const Workspace* workspace = view->workspace()) {
      if (workspace->layout().columnOf(view) >= 0) {
        const wlr_box target = workspace->layout().targetBox(view);
        if (target.width > 0 && target.height > 0) {
          return target;
        }
      }
    }
    return {view->sceneTree()->node.x, view->sceneTree()->node.y, geometry.width, geometry.height};
  }

  void Overview::layoutCard(Card& card, const RowMetrics& metrics, double rowScroll) {
    View* view = card.view;
    const wlr_box& geometry = view->toplevel()->base->geometry;
    if (geometry.width <= 0 || geometry.height <= 0) {
      card.blur.hide();
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
    const float cardOpacity = &card == m_dragCard ? View::kDragOpacity : 1.0F;

    // Rows overhang the output box by design (adjacent workspaces peek in), so
    // every piece is clipped to the owning output; on a stacked multi-head
    // layout the overhang would otherwise bleed onto the neighbour.
    const bool clipped = &card != m_dragCard;
    wlr_box visible = card.box;
    if (clipped && !wlr_box_intersection(&visible, &card.box, &metrics.outputBox)) {
      card.blur.hide();
      wlr_scene_node_set_enabled(&card.tree->node, false);
      return;
    }

    const auto& appearance = config().appearance;
    const int total = appearance.totalBorderWidth();
    const bool decorated = total > 0 && !view->toplevel()->current.fullscreen;
    // Scale the radius and each thickness once, then derive the rings from those
    // scaled values. Rounding the outer radius on its own drifts from the ring's
    // own thickness at fractional zoom, so the curve stops matching the stroke.
    const int radius = decorated ? static_cast<int>(std::lround(appearance.cornerRadius * z)) : 0;
    const wlr_box* clip = clipped ? &metrics.outputBox : nullptr;
    const auto paintRing = [&](wlr_scene_rect* rect, int thickness, const std::array<float, 4>& base) {
      if (rect == nullptr) {
        return;
      }
      const int scaled = thickness > 0 ? std::max(1, static_cast<int>(std::lround(thickness * z))) : 0;
      const bool visible = decorated
          && scaled > 0
          && applyBorderRing(rect, makeBorderRing(contentW, contentH, radius, scaled), card.box.x, card.box.y, clip);
      wlr_scene_node_set_enabled(&rect->node, visible);
      if (visible) {
        const std::array<float, 4> color = tint(base, cardOpacity);
        wlr_scene_rect_set_color(rect, color.data());
      }
    };
    // The outer ring spans both widths so the inner ring tucks into it without a
    // seam, exactly as ViewDecoration draws a real window.
    paintRing(card.outerBorder, appearance.outerBorderWidth > 0 ? total : 0, appearance.outerBorderColor);
    // Every row advertises its own focused window, not just the active one: the
    // filmstrip is a browsing aid, and the border is what tells you where each
    // workspace will land you when you zoom into it.
    const Workspace* workspace = view->workspace();
    const bool focused = workspace != nullptr && workspace->focusedView() == view && m_dragCard != &card;
    paintRing(card.border, appearance.borderWidth, focused ? appearance.borderFocused : appearance.borderUnfocused);

    const double fx = static_cast<double>(contentW) / geometry.width;
    const double fy = static_cast<double>(contentH) / geometry.height;
    bool blurUpdated = false;
    for (const auto& entry : card.surfaces) {
      wlr_surface* surface = entry->surface;
      if (entry->buffer == nullptr || surface->current.width <= 0 || surface->current.height <= 0) {
        continue;
      }
      const float surfaceOpacity =
          entry->sourceBuffer != nullptr ? entry->sourceBuffer->opacity * cardOpacity : cardOpacity;
      wlr_scene_buffer_set_opacity(entry->buffer, surfaceOpacity);
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
      // A card cut by the output edge drops the radius on the cut corners, the
      // same rule View applies to a window that spans outputs.
      wlr_scene_buffer_set_corner_radii(
          entry->buffer, cornerRadiiForVisible(card.box, visible, corner_radii_all(radius))
      );
      const wlr_box blurBox{0, 0, contentW, contentH};
      const wlr_box blurClip{
          visible.x - card.box.x,
          visible.y - card.box.y,
          visible.width,
          visible.height,
      };
      card.blur.setAlpha(entry->buffer->opacity);
      card.blur.update(card.tree, surface, blurBox, geometry, radius, &blurClip, view->blurOptions(), entry->buffer);
      blurUpdated = true;
    }
    if (!blurUpdated) {
      card.blur.hide();
    }
  }

  void Overview::layoutOutput(OutputState& state) {
    RowMetrics metrics{};
    if (!rowMetrics(state, *m_server, zoom(), metrics)) {
      return;
    }

    wlr_scene_node_set_position(&state.backgroundTint->node, metrics.outputBox.x, metrics.outputBox.y);
    wlr_scene_rect_set_size(state.backgroundTint, metrics.outputBox.width, metrics.outputBox.height);
    const std::array<float, 4> backgroundTint = tint(config().overview.backgroundTint, m_progress);
    // A fully transparent tint leaves the wallpaper untouched.
    wlr_scene_node_set_enabled(&state.backgroundTint->node, backgroundTint[3] > 0.001F);
    wlr_scene_rect_set_color(state.backgroundTint, backgroundTint.data());

    const int backgroundRadius = static_cast<int>(std::lround(config().appearance.cornerRadius * metrics.zoom));
    const std::array<float, 4> backgroundColor = tint(config().overview.workspaceBackground, m_progress);
    for (size_t row = 0; row < state.workspaceBackgrounds.size(); ++row) {
      wlr_scene_rect* background = state.workspaceBackgrounds[row];
      const wlr_box full{metrics.rowX, rowTop(metrics, state.rowScroll, row), metrics.rowW, metrics.rowH};
      layoutWorkspaceBackground(background, full, metrics.outputBox, backgroundRadius, backgroundColor);
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

  void Overview::syncCardBuffer(CardSurface& entry) {
    wlr_surface* surface = entry.surface;
    if (surface == nullptr || entry.buffer == nullptr) {
      return;
    }

    wlr_scene_buffer_set_buffer_options options{
        .damage = &surface->buffer_damage,
        .wait_timeline = nullptr,
        .wait_point = 0,
    };
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
      options.wait_timeline = sync->acquire_timeline;
      options.wait_point = sync->acquire_point;
    }
    // A scene buffer may clear its `buffer` pointer after importing a texture
    // and releasing the client buffer. The surface retains the authoritative
    // committed buffer, including for hidden workspaces.
    wlr_buffer* committed = surface->buffer != nullptr ? &surface->buffer->base : nullptr;
    if (committed == nullptr) {
      options.damage = nullptr;
    }
    wlr_scene_buffer_set_buffer_with_options(entry.buffer, committed, &options);

    // Presentation state comes from the surface's committed state, NEVER from
    // the view's scene buffers: workspace slides clip those (setOutputClip →
    // applyPresentedCrop), and a window parked on a hidden workspace keeps the
    // final sliver crop. Copying it smears subsurface-presented content (games)
    // into a single stretched line. layoutCard then re-crops the root surface
    // and re-scales every entry for the thumbnail.
    wlr_fbox src{};
    wlr_surface_get_buffer_source_box(surface, &src);
    wlr_scene_buffer_set_source_box(entry.buffer, &src);
    wlr_scene_buffer_set_dest_size(entry.buffer, surface->current.width, surface->current.height);
    wlr_scene_buffer_set_transform(entry.buffer, surface->current.transform);
    wlr_scene_buffer_set_opaque_region(entry.buffer, &surface->opaque_region);

    // Protocol-derived display properties (alpha-modifier, color management)
    // are clip-independent, so the view's scene buffer is a safe source.
    if (wlr_scene_buffer* source = entry.sourceBuffer) {
      wlr_scene_buffer_set_opacity(entry.buffer, source->opacity);
      wlr_scene_buffer_set_transfer_function(entry.buffer, source->transfer_function);
      wlr_scene_buffer_set_primaries(entry.buffer, source->primaries);
      wlr_scene_buffer_set_color_encoding(entry.buffer, source->color_encoding);
      wlr_scene_buffer_set_color_range(entry.buffer, source->color_range);
    }
  }

  void Overview::addCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    auto* card = static_cast<Card*>(data);
    wlr_scene_buffer* source = sourceBufferForSurface(&card->view->sceneTree()->node, surface);
    if (source == nullptr) {
      return;
    }
    wlr_scene_buffer* buffer = wlr_scene_buffer_create(card->tree, nullptr);
    if (buffer == nullptr) {
      return;
    }
    auto entry = std::make_unique<CardSurface>();
    entry->card = card;
    entry->surface = surface;
    entry->sourceBuffer = source;
    entry->buffer = buffer;
    entry->sx = sx;
    entry->sy = sy;
    entry->isRoot = surface == card->view->toplevel()->base->surface;
    wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
    buffer->point_accepts_input = rejectInput;
    entry->commit.notify = onCardSurfaceCommit;
    wl_signal_add(&surface->events.commit, &entry->commit);
    entry->destroy.notify = onCardSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &entry->destroy);
    entry->outputSample.notify = onCardBufferOutputSample;
    wl_signal_add(&buffer->events.output_sample, &entry->outputSample);
    entry->frameDone.notify = onCardBufferFrameDone;
    wl_signal_add(&buffer->events.frame_done, &entry->frameDone);
    syncCardBuffer(*entry);
    card->surfaces.push_back(std::move(entry));
    if (card->outerBorder != nullptr) {
      wlr_scene_node_raise_to_top(&card->outerBorder->node);
    }
    if (card->border != nullptr) {
      wlr_scene_node_raise_to_top(&card->border->node);
    }
  }

  void Overview::syncCardSurface(wlr_surface* surface, int sx, int sy, void* data) {
    auto* card = static_cast<Card*>(data);
    for (const auto& entry : card->surfaces) {
      if (entry->surface == surface) {
        entry->sx = sx;
        entry->sy = sy;
        syncCardBuffer(*entry);
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
    // The source scene surface reconfigures on every commit. Refresh the
    // passive buffer mirrors, then re-derive their overview crop and scale.
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
    wl_list_remove(&entry->outputSample.link);
    wl_list_remove(&entry->frameDone.link);
    if (entry->buffer != nullptr) {
      wlr_scene_node_destroy(&entry->buffer->node);
      entry->buffer = nullptr;
    }
    std::erase_if(card->surfaces, [entry](const std::unique_ptr<CardSurface>& candidate) {
      return candidate.get() == entry;
    });
  }

  void Overview::onCardBufferOutputSample(wl_listener* listener, void* data) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, outputSample);
    auto* event = static_cast<wlr_scene_output_sample_event*>(data);
    wlr_output* output = event->output->output;
    if (event->direct_scanout) {
      wlr_presentation_surface_scanned_out_on_output(entry->surface, output);
    } else {
      wlr_presentation_surface_textured_on_output(entry->surface, output);
    }
    if (wlr_linux_drm_syncobj_surface_v1_state* sync = wlr_linux_drm_syncobj_v1_get_surface_state(entry->surface);
        sync != nullptr && event->release_timeline != nullptr) {
      wlr_linux_drm_syncobj_v1_state_add_release_point(
          sync, event->release_timeline, event->release_point, output->event_loop
      );
    }
  }

  void Overview::onCardBufferFrameDone(wl_listener* listener, void* data) {
    CardSurface* entry;
    entry = wl_container_of(listener, entry, frameDone);
    auto* event = static_cast<wlr_scene_frame_done_event*>(data);
    wlr_surface_send_frame_done(entry->surface, &event->when);
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
    // Outer first so the inner ring, which carries the focus colour, draws over
    // it. The punched hole keeps both above the card buffers.
    const std::array<float, 4> outerColor = tint(config().appearance.outerBorderColor, 1.0);
    card->outerBorder = wlr_scene_rect_create(card->tree, 1, 1, outerColor.data());
    const std::array<float, 4> borderColor = tint(config().appearance.borderUnfocused, 1.0);
    card->border = wlr_scene_rect_create(card->tree, 1, 1, borderColor.data());
    Card* raw = card.get();
    state.cards.push_back(std::move(card));

    wlr_surface_for_each_surface(surface, addCardSurface, raw);

    // Animation schedules the first output frame. The passive card buffers then
    // pace clients from frames where their content was actually sampled.
    return raw;
  }

  void Overview::destroyCard(Card* card) {
    for (const auto& entry : card->surfaces) {
      wl_list_remove(&entry->commit.link);
      wl_list_remove(&entry->destroy.link);
      wl_list_remove(&entry->outputSample.link);
      wl_list_remove(&entry->frameDone.link);
    }
    card->surfaces.clear();
    if (card->tree != nullptr) {
      wlr_scene_node_destroy(&card->tree->node);
      card->tree = nullptr;
    }
    card->outerBorder = nullptr;
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
          const bool fullscreen = view->toplevel()->current.fullscreen;
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
    for (const auto& output : m_server->outputs()) {
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
      const std::array<float, 4> backgroundTint = tint(config().overview.backgroundTint, 0.0);
      state->backgroundTint = wlr_scene_rect_create(state->tree, 1, 1, backgroundTint.data());
      wlr_scene_rect_set_corner_radius(state->backgroundTint, 0);
      state->workspaceBackgrounds.reserve(group->workspaceCount());
      const std::array<float, 4> backgroundColor = tint(config().overview.workspaceBackground, 0.0);
      for (size_t row = 0; row < group->workspaceCount(); ++row) {
        state->workspaceBackgrounds.push_back(wlr_scene_rect_create(state->tree, 1, 1, backgroundColor.data()));
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
    // A data-device drag owns wlroots' pointer and keyboard grabs until the
    // initiating button is released. Taking overview input ownership now would
    // hide that release from the drag and leave both grabs active.
    if (m_server->seat()->wlr()->drag != nullptr) {
      kLog.debug("overview open ignored during active client drag");
      return false;
    }
    m_server->cursor()->resetMode();
    for (const auto& output : m_server->outputs()) {
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
    m_anim.snap(0.0);
    if (m_dragCard != nullptr) {
      endDrag(false);
    }
    m_pendingFocus = nullptr;
    teardown();
    m_server->refocus();
  }

  void Overview::startAnimation(double target, bool closing) {
    m_closing = closing;
    m_targetProgress = target;
    m_progressFrom = m_progress;
    m_anim.snap(0.0);
    m_anim.retarget(1.0, std::max(1, config().appearance.animationMs), Easing::EaseOutCubic);
    // Animations only tick from an output frame; kick one so the zoom starts on
    // an idle desktop (the value itself clocks from its first tick).
    scheduleFrames();
  }

  bool Overview::tickAnimations(uint64_t nowMsec) {
    bool active = m_dropHint != nullptr && m_dropHint->tickAnimations(nowMsec);
    if (m_anim.tick(nowMsec)) {
      const double value = m_anim.current();
      m_progress = m_progressFrom + (m_targetProgress - m_progressFrom) * value;
      for (const auto& state : m_outputs) {
        state->rowScroll = state->rowFrom + (state->rowTo - state->rowFrom) * value;
      }
      applyProgress();
      if (!m_anim.animating()) {
        // May tear down m_outputs; safe now that the loop above is done.
        finishAnimation();
      }
      active = m_anim.animating() || active;
    }
    return active;
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
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        if (Workspace* workspace = group->active()) {
          workspace->markArrange(false);
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
    if (m_dropHint != nullptr) {
      m_dropHint->hideImmediate();
    }
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
    m_dragSourceWidth.reset();
    m_drop = {};
    m_gestureOpenedHere = false;
    m_server->reconcileDynamicWorkspaces();
  }

  // ----------------------------------------------------------------- gesture

  void Overview::gestureUpdate(double progress) {
    if (!m_active) {
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedHere = true;
    }
    m_anim.snap(0.0);
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
    if (m_drop.view == view) {
      m_drop.view = nullptr;
      m_drop.edge = 0;
      hideDropHint();
    }
    if (m_dragCard != nullptr && m_dragCard->view == view) {
      hideDropHint();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_drop = {};
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
    if (std::abs(target->rowTo - row) < 0.001 && m_anim.animating()) {
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
    if (m_drop.workspace != nullptr && stateForWorkspace(m_drop.workspace) == it->get()) {
      hideDropHint();
      m_drop = {};
    }
    if (m_dragCard != nullptr && m_dragCard->owner == it->get()) {
      hideDropHint();
      m_dragCard = nullptr;
      m_dragSourceWorkspace = nullptr;
      m_drop = {};
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
      for (size_t row = 0; row < state->workspaceBackgrounds.size(); ++row) {
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

  bool Overview::selectRelativeWorkspace(int delta, Output* output) {
    if (!interactive()) {
      return false;
    }
    if (output == nullptr) {
      output = m_server->outputFromWlr(m_server->preferredOutput());
    }
    if (output == nullptr || output->workspaceGroup() == nullptr) {
      return false;
    }
    WorkspaceGroup* group = output->workspaceGroup();
    if (group->active() == nullptr) {
      return false;
    }
    const int index = static_cast<int>(group->active()->index()) + delta;
    if (index < 0 || index >= static_cast<int>(group->workspaceCount())) {
      return false;
    }
    // select() lands in onWorkspaceActivated, which animates rowScroll onto the
    // new row, so the filmstrip follows without the caller arranging anything.
    group->select(group->workspaceAt(static_cast<size_t>(index)));
    return true;
  }

  bool Overview::handleAxisNotch(bool vertical, double direction, double lx, double ly) {
    if (!interactive() || !vertical) {
      return true;
    }
    wlr_output* wlrOutput = wlr_output_layout_output_at(m_server->outputLayout(), lx, ly);
    selectRelativeWorkspace(direction < 0 ? -1 : 1, m_server->outputFromWlr(wlrOutput));
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
    case XKB_KEY_Down:
      selectRelativeWorkspace(keysym == XKB_KEY_Up ? -1 : 1, nullptr);
      return true;
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
    m_dragSourceWidth.reset();
    m_drop = {};

    if (m_dragSourceWorkspace != nullptr && view->tiled()) {
      m_dragSourceColumn = m_dragSourceWorkspace->layout().columnOf(view);
      m_dragSourceRow = m_dragSourceWorkspace->layout().rowOf(view);
      m_dragSourceWidth = captureDropColumnWidth(*m_dragSourceWorkspace, view);
      if (m_dragSourceColumn >= 0) {
        // Detach so the source row closes the gap live, exactly like a normal
        // tile drag; arrange() re-lays that output's cards through the hook.
        m_dragSourceWorkspace->layoutDetach(view, false);
      }
    }
    wlr_scene_node_reparent(&card->tree->node, m_tree);
    wlr_scene_node_raise_to_top(&card->tree->node);
    RowMetrics metrics{};
    if (card->owner != nullptr && rowMetrics(*card->owner, *m_server, zoom(), metrics)) {
      layoutCard(*card, metrics, card->owner->rowScroll);
    }
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
    m_drop = {.workspace = workspace};
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

    if (card->view->tiled()) {
      m_drop = computeDropTarget(*workspace, worldX, worldY, card->view);
    } else {
      m_drop = {
          .workspace = workspace,
          .column = static_cast<int>(workspace->layout().columns().size()),
      };
    }
    if (m_drop.hintBox.width > 0 && m_drop.hintBox.height > 0) {
      showDropHint(m_drop.hintBox, metrics, state->rowScroll, row, state->output);
    } else {
      hideDropHint();
    }
    scheduleFrames();
  }

  void Overview::endDrag(bool drop) {
    Card* card = m_dragCard;
    if (card == nullptr) {
      return;
    }
    View* view = card->view;
    Workspace* target = drop ? m_drop.workspace : nullptr;
    const DropTarget targetDrop = m_drop;
    const wlr_box cardBox = card->box;
    OutputState* dropState = target != nullptr ? stateForWorkspace(target) : nullptr;

    m_dragCard = nullptr;
    m_drop = {};
    hideDropHint();
    m_server->cursor()->overrideCursor(nullptr);

    if (view == nullptr || !view->mapped()) {
      return;
    }

    if (target != nullptr && view->tiled()) {
      applyDrop(
          *m_server, *view, *target, targetDrop, m_dragSourceWidth.has_value() ? &*m_dragSourceWidth : nullptr,
          /*animate=*/false
      );
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
      if (m_dragSourceWidth.has_value()) {
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
        const int column = m_dragSourceWorkspace->layout().columnOf(view);
        m_dragSourceWorkspace->layout().setWidthFraction(column, m_dragSourceWidth->fraction);
        if (m_dragSourceWidth->fullWidth) {
          m_dragSourceWorkspace->layout().toggleFullWidth(column);
        }
        wlr_xdg_toplevel_set_maximized(view->toplevel(), m_dragSourceWidth->fullWidth);
      } else if (m_dragSourceRow >= 0) {
        m_dragSourceWorkspace->layout().insertViewIntoColumn(view, m_dragSourceColumn, m_dragSourceRow);
      } else {
        m_dragSourceWorkspace->layout().insertView(view, m_dragSourceColumn);
      }
      m_dragSourceWorkspace->arrange(false);
    }

    m_dragSourceWorkspace = nullptr;
    m_dragSourceColumn = -1;
    m_dragSourceRow = -1;
    m_dragSourceWidth.reset();
    // The card moved rows and scene parents; rebuilding is cheaper to reason
    // about than rebinding its surface listeners in place.
    rebuildCard(view);
    applyProgress();
  }

  void Overview::showDropHint(
      const wlr_box& worldBox, const RowMetrics& metrics, double rowScroll, size_t row, Output* output
  ) {
    if (worldBox.width <= 0 || worldBox.height <= 0) {
      hideDropHint();
      return;
    }
    const double z = metrics.zoom;
    const wlr_box mappedBox{
        .x = metrics.rowX + static_cast<int>(std::lround((worldBox.x - metrics.outputBox.x) * z)),
        .y = rowTop(metrics, rowScroll, row) + static_cast<int>(std::lround((worldBox.y - metrics.outputBox.y) * z)),
        .width = std::max(1, static_cast<int>(std::lround(worldBox.width * z))),
        .height = std::max(1, static_cast<int>(std::lround(worldBox.height * z))),
    };
    if (m_dropHint == nullptr) {
      m_dropHint = std::make_unique<HintRect>(*m_server, m_tree);
    }
    m_dropHint->show(output, mappedBox, static_cast<int>(std::lround(config().appearance.cornerRadius * metrics.zoom)));
    if (m_dragCard != nullptr && m_dragCard->tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_dragCard->tree->node);
    }
  }

  void Overview::hideDropHint() {
    if (m_dropHint != nullptr) {
      m_dropHint->hide();
    }
  }

} // namespace umbriel
