/*
 * hitz_log_gate.h - the launcher's "Debug logging" switch, for every print.
 *
 * Force-included (CMakeLists.txt: /FI) into the toolkit libraries and the
 * game's own source files, but not the generated code. With logging off
 * (g_hitz_log == 0, the default; HITZ_DEBUG=1 turns it on) a print to
 * stderr or stdout is skipped before its arguments are formatted, so the
 * ~800 log lines in the toolkit and the game cost one test each. Prints to
 * other files (the shader cache, the performance log) are untouched.
 */
#ifndef HITZ_LOG_GATE_H
#define HITZ_LOG_GATE_H

#include <stdio.h>
#include <stdarg.h>

/* 1 = debug logging on; main.c sets it from HITZ_DEBUG. Defined here, once
 * per program (selectany / weak), so toolkit test programs that link the
 * libraries without main.c still link. */
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_MSC_VER)
__declspec(selectany) volatile int g_hitz_log = 0;
#else
__attribute__((weak)) volatile int g_hitz_log = 0;
#endif
#ifdef __cplusplus
}
#endif

#define HITZ_LOG_SKIP(f) (!g_hitz_log && ((f) == stderr || (f) == stdout))

#define fprintf(f, ...)        (HITZ_LOG_SKIP(f) ? 0 : (fprintf)((f), __VA_ARGS__))
#define vfprintf(f, fmt, ap)   (HITZ_LOG_SKIP(f) ? 0 : (vfprintf)((f), (fmt), (ap)))
#define fputs(s, f)            (HITZ_LOG_SKIP(f) ? 0 : (fputs)((s), (f)))
#define fputc(c, f)            (HITZ_LOG_SKIP(f) ? 0 : (fputc)((c), (f)))
#define fwrite(p, sz, n, f)    (HITZ_LOG_SKIP(f) ? (size_t)0 : (fwrite)((p), (sz), (n), (f)))
#define printf(...)            (!g_hitz_log ? 0 : (printf)(__VA_ARGS__))
#define vprintf(fmt, ap)       (!g_hitz_log ? 0 : (vprintf)((fmt), (ap)))
#define puts(s)                (!g_hitz_log ? 0 : (puts)(s))

#endif /* HITZ_LOG_GATE_H */
