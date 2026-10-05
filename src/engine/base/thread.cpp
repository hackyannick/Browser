#include "base/thread.h"

namespace kite {

namespace {
WakeUiFunc g_waker = 0;
void* g_wakerCtx = 0;
}  // namespace

void SetUiWaker(WakeUiFunc fn, void* ctx) {
  g_waker = fn;
  g_wakerCtx = ctx;
}

void WakeUi() {
  if (g_waker) g_waker(g_wakerCtx);
}

}  // namespace kite
