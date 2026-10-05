QuickJS (https://bellard.org/quickjs, MIT license), version in VERSION.

Local changes for Kite:
- quickjs.c: CONFIG_ATOMICS is disabled on _WIN32 (Atomics.wait needs
  pthreads, which do not exist on Windows 2000).
- quickjs.c/h: JS_EVAL_FLAG_NO_RESOLVE and JS_GetModuleRequestCount()/
  JS_GetModuleRequest(), so that the browser can fetch a module graph
  asynchronously before linking it.
