/* Hand-written dav1d configuration for Kite (no assembly, 8 and 10/12 bit). */
#ifndef KITE_DAV1D_CONFIG_H
#define KITE_DAV1D_CONFIG_H

#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
#define CONFIG_LOG 0
#define ENDIANNESS_BIG 0
#define HAVE_ASM 0
#define HAVE_C11_GENERIC 1
#define TRIM_DSP_FUNCTIONS 1

#ifdef _WIN32
#define HAVE_IO_H 1
#define UNICODE 1
#define _UNICODE 1
#define __USE_MINGW_ANSI_STDIO 1
#else
#define HAVE_CLOCK_GETTIME 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_UNISTD_H 1
#endif

#endif
