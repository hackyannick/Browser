# libwebp in Kite

Decoder-only subset of libwebp 1.3.2 (Debian/Ubuntu source package
1.3.2-0.4build3, which includes the security fixes). Encoder, MIPS, NEON
and MSA sources were removed. Threading (WEBP_USE_THREAD) is not enabled,
SSE2/SSE4.1 paths are only compiled when the compiler targets them, so the
32-bit Windows build runs on CPUs without SSE2 (Windows 2000 era).
