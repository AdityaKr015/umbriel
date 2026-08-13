#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace umbriel {

  class View;

  // Owns every View, ordered most-recently-focused first.
  //
  // The order is the whole point of this class. The front entry is the window
  // focused last, which is what focus falls back to when the current window
  // closes, when a session unlocks, or when a workspace empties — and cycling
  // through windows walks the same list. Keeping the order here means the
  // ordering rules live in one place instead of being re-derived by hand at each
  // of those call sites.
  class ViewRegistry {
  public:
    ViewRegistry();
    ~ViewRegistry();

    ViewRegistry(const ViewRegistry&) = delete;
    ViewRegistry& operator=(const ViewRegistry&) = delete;

    // New windows go to the back: an unfocused window is by definition the least
    // recently focused one. Focusing it promotes it.
    View& add(std::unique_ptr<View> view);
    void remove(View* view);
    void clear();

    [[nodiscard]] std::span<const std::unique_ptr<View>> all() const { return m_views; }
    [[nodiscard]] bool empty() const { return m_views.empty(); }
    [[nodiscard]] View* mostRecent() const { return m_views.empty() ? nullptr : m_views.front().get(); }

    // Move `view` to the front. No-op when it is absent or already there.
    void promote(View* view);

    // Give each entry a turn at the front until one satisfies `accept`, and
    // return it. Returns nullptr when nothing does, in which case the order is
    // unchanged — a full rotation is the identity.
    View* rotateToNext(const std::function<bool(const View&)>& accept);

  private:
    std::vector<std::unique_ptr<View>> m_views;
  };

} // namespace umbriel
