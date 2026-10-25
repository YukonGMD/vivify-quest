#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace Vivify {

// Strong GC handles on the Unity objects Vivify keeps (0.14.25-naomi.10).
//
// Vivify stores Unity objects -- prefab instances, the game's note renderers
// it hides, the copies it spawns, bundle assets -- as plain pointers in its own
// C++ containers, which il2cpp's garbage collector does not scan. A wrapper
// nothing else in managed code refers to (because the object was destroyed, or
// because Unity holds its native side without one) can then be freed, and the
// next liveness check reads freed memory, says "alive", and the call after it
// crashes. naomi.9 crashed that way twice during level teardown: Renderer
// .set_enabled on a freed note renderer, Object.Destroy on a freed prefab
// instance. A pinned wrapper stays valid; once its object is destroyed, Unity
// clears it and the liveness check says so.
//
// Handles are taken once per object and released all at once: level objects
// at the end of a level's teardown, bundle assets when the bundle goes.
class ObjectPins {
 public:
  using NewHandle = uint32_t (*)(void* object);
  using FreeHandle = void (*)(uint32_t handle);

  // The default functions are il2cpp's (gchandle_new, not pinned in memory,
  // and gchandle_free); tests pass their own.
  ObjectPins();
  ObjectPins(NewHandle newHandle, FreeHandle freeHandle);
  ~ObjectPins();
  ObjectPins(ObjectPins const&) = delete;
  ObjectPins& operator=(ObjectPins const&) = delete;

  // Null is ignored; an object already pinned is not pinned twice.
  void Pin(void const* object);
  void ReleaseAll();
  size_t Count() const { return _handles.size(); }

 private:
  NewHandle _new;
  FreeHandle _free;
  std::unordered_map<void const*, uint32_t> _handles;
};

}  // namespace Vivify
