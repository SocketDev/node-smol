// Native TextBufferView lifecycle + geometry/indicator config.
//
// Port of the lifecycle + config-setter subset of stuie's
// crates/stuie-cabi/src/text_view.rs (slice S5). See tui/text_view.hpp for the
// scope and registry-placement notes. The view registry and the per-buffer
// owned-child index live here (mirroring stuie's TBV_REGISTRY /
// OWNED_CHILD_VIEWS), so the owner-destroy cascade and borrowed-buffer
// rejection are self-contained and testable without the V8 binding.

#include "tui/text_view.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tui/handles.hpp"  // handles::Tag, kKindTextBufferView

namespace tui {

namespace {

// The view registry: handle -> TextBufferView (text_view.rs:841-858
// TbvRegistry). A function-local static so first use initializes it, matching
// stuie's OnceLock. The mutex guards concurrent access exactly like stuie's
// Mutex<TbvRegistry>.
struct TbvRegistry {
  std::mutex mu;
  // Monotonic 1-based index folded into the kind-tagged handle. Never reused
  // within a process run, so a stale handle never aliases a fresh one.
  uint32_t next = 1;
  std::unordered_map<uint32_t, std::unique_ptr<TextBufferView>> map;
};

TbvRegistry& Registry() {
  static TbvRegistry r;
  return r;
}

// The owned-child index: buffer handle -> the external view handles created
// over it via createTextBufferView (text_view.rs:891-898 OWNED_CHILD_VIEWS).
// Views an EditorView creates internally are owned by the EditorView, not the
// buffer, so they are NOT tracked here.
struct OwnedChildViews {
  std::mutex mu;
  std::unordered_map<uint32_t, std::vector<uint32_t>> map;
};

OwnedChildViews& OwnedChildren() {
  static OwnedChildViews r;
  return r;
}

}  // namespace

uint32_t TbvCreate(uint32_t tb) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  uint32_t handle = handles::Tag(handles::kKindTextBufferView, r.next);
  // Advance the monotonic index (wrap to 1, never 0), mirroring
  // reg.next.wrapping_add(1).max(1) (text_view.rs:870).
  r.next += 1;
  if (r.next == 0) {
    r.next = 1;
  }
  r.map.emplace(handle, std::make_unique<TextBufferView>(tb));
  return handle;
}

uint32_t TbvCreateOwned(uint32_t tb, bool buffer_borrowed) {
  // A borrowed text buffer (an EditBuffer's inner buffer) cannot own external
  // views; return the 0 "failed" handle (the shim then throws), mirroring
  // createTextBufferView's is_borrowed gate (cabi.rs:1391-1406).
  if (buffer_borrowed) {
    return 0;
  }
  uint32_t handle = TbvCreate(tb);
  if (handle != 0) {
    RegisterOwnedChild(tb, handle);
  }
  return handle;
}

bool TbvIsValid(uint32_t handle) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  return r.map.find(handle) != r.map.end();
}

void TbvDestroy(uint32_t handle) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.map.erase(handle);
}

void RegisterOwnedChild(uint32_t tb, uint32_t view) {
  OwnedChildViews& r = OwnedChildren();
  std::lock_guard<std::mutex> lock(r.mu);
  r.map[tb].push_back(view);
}

void DestroyOwnedChildren(uint32_t tb) {
  // Take the child list out under the owned-child lock, then destroy each view.
  // TbvDestroy takes the view-registry lock separately, so the two locks are
  // never held at once (no lock-order coupling). text_view.rs:911-918.
  std::vector<uint32_t> children;
  {
    OwnedChildViews& r = OwnedChildren();
    std::lock_guard<std::mutex> lock(r.mu);
    auto it = r.map.find(tb);
    if (it == r.map.end()) {
      return;
    }
    children = std::move(it->second);
    r.map.erase(it);
  }
  for (uint32_t view : children) {
    TbvDestroy(view);
  }
}

namespace {

// with_tbv (text_view.rs:860-865): run `fn` against the live view for `handle`,
// or do nothing for a stale/wrong-kind handle. The lock is held for the call.
template <typename Fn>
void WithTbv(uint32_t handle, Fn&& fn) {
  TbvRegistry& r = Registry();
  std::lock_guard<std::mutex> lock(r.mu);
  auto it = r.map.find(handle);
  if (it != r.map.end()) {
    fn(*it->second);
  }
}

}  // namespace

void TbvSetWrapMode(uint32_t handle, uint8_t mode) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetWrapMode(mode); });
}

void TbvSetWrapWidth(uint32_t handle, uint32_t width) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetWrapWidth(width); });
}

void TbvSetFirstLineOffset(uint32_t handle, uint32_t offset) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetFirstLineOffset(offset); });
}

void TbvSetViewport(uint32_t handle, uint32_t x, uint32_t y, uint32_t width,
                    uint32_t height) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetViewport(x, y, width, height); });
}

void TbvSetViewportSize(uint32_t handle, uint32_t width, uint32_t height) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetViewportSize(width, height); });
}

void TbvSetTruncate(uint32_t handle, bool truncate) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetTruncate(truncate); });
}

void TbvSetTabIndicator(uint32_t handle, uint32_t code_point) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetTabIndicator(code_point); });
}

void TbvSetTabIndicatorColor(uint32_t handle,
                             std::optional<std::array<uint16_t, 4>> color) {
  WithTbv(handle, [&](TextBufferView& v) { v.SetTabIndicatorColor(color); });
}

}  // namespace tui
