# dav1d in Kite

dav1d 1.4.1 (BSD-2-Clause, from the Debian/Ubuntu source package), used to
decode AVIF images. Only the portable C sources are vendored (no assembly).

Kite changes:

- `config/config.h` and `config/vcs_version.h` replace the Meson generated
  files (C only, 8 and 16 bit pixel depths, logging off).
- `src/thread.h`, `src/win32/thread.c`: SRW locks, condition variables and
  InitOnce do not exist on Windows 2000; replaced by small interlocked
  implementations. Kite runs the decoder single-threaded.
- `src/mem.h`: `_aligned_malloc` is missing from the Windows 2000 msvcrt;
  uses MinGW's `__mingw_aligned_malloc` instead.
- `src/cpu.c`: processor count via `GetSystemInfo` (`GetThreadGroupAffinity` is Windows 7+).
