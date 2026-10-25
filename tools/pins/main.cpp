// Host test for src/VivifyPins.cpp: the bookkeeping of Vivify's strong GC
// handles, with fake handle functions. (What the handles do -- keep il2cpp's
// collector off an object -- can only be seen on the headset.)
//
//   g++ -std=c++20 -O1 -g -fsanitize=address,undefined -DVIVIFY_HOST_TEST -I include \
//       -o /tmp/pins tools/pins/main.cpp src/VivifyPins.cpp && /tmp/pins
#include "VivifyPins.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace {

int gFailures = 0;
void Check(char const* name, bool ok, std::string const& detail = "") {
  std::printf("%s %s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : (" " + detail).c_str());
  if (!ok) gFailures++;
}

uint32_t gNext = 1;
std::map<uint32_t, void*> gLive;  // handle -> object
uint32_t FakeNew(void* object) {
  gLive[gNext] = object;
  return gNext++;
}
void FakeFree(uint32_t handle) {
  gLive.erase(handle);
}
uint32_t RefusingNew(void*) {
  return 0;
}

}  // namespace

int main() {
  int a = 0, b = 0;
  {
    Vivify::ObjectPins pins(&FakeNew, &FakeFree);
    pins.Pin(&a);
    pins.Pin(&b);
    pins.Pin(&a);
    pins.Pin(nullptr);
    Check("each object is pinned once, null is ignored", pins.Count() == 2 && gLive.size() == 2,
          std::to_string(gLive.size()));
    pins.ReleaseAll();
    Check("releasing frees every handle", pins.Count() == 0 && gLive.empty(), std::to_string(gLive.size()));
    pins.Pin(&a);
    Check("an object can be pinned again after a release", pins.Count() == 1 && gLive.size() == 1);
  }
  Check("the destructor frees what is left", gLive.empty(), std::to_string(gLive.size()));
  {
    Vivify::ObjectPins pins(&RefusingNew, &FakeFree);
    pins.Pin(&a);
    Check("a handle the runtime refuses is not recorded", pins.Count() == 0);
  }
  std::printf("\n%d failure(s)\n", gFailures);
  return gFailures == 0 ? 0 : 1;
}
