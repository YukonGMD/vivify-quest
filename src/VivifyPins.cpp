#include "VivifyPins.hpp"

#ifndef VIVIFY_HOST_TEST
// Ahead of beatsaber-hook, which reaches paper2's backtrace.hpp (see main.hpp).
#include <fstream>
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#endif

namespace Vivify {

namespace {

#ifndef VIVIFY_HOST_TEST
uint32_t Il2cppNewHandle(void* object) {
  il2cpp_functions::Init();
  return il2cpp_functions::gchandle_new(static_cast<Il2CppObject*>(object), false);
}

void Il2cppFreeHandle(uint32_t handle) {
  il2cpp_functions::Init();
  il2cpp_functions::gchandle_free(handle);
}
#else
uint32_t Il2cppNewHandle(void*) {
  return 0;
}
void Il2cppFreeHandle(uint32_t) {}
#endif

}  // namespace

ObjectPins::ObjectPins() : ObjectPins(&Il2cppNewHandle, &Il2cppFreeHandle) {}

ObjectPins::ObjectPins(NewHandle newHandle, FreeHandle freeHandle) : _new(newHandle), _free(freeHandle) {}

ObjectPins::~ObjectPins() {
  ReleaseAll();
}

void ObjectPins::Pin(void const* object) {
  if (object == nullptr || _handles.contains(object)) return;
  uint32_t const handle = _new(const_cast<void*>(object));
  if (handle != 0) _handles.emplace(object, handle);
}

void ObjectPins::ReleaseAll() {
  for (auto const& [object, handle] : _handles) _free(handle);
  _handles.clear();
}

}  // namespace Vivify
