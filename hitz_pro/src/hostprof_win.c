/*
 * hostprof_win.c -- in-process sampling profiler for the Windows build.
 *
 * HITZ_HOSTPROF=<file> starts a thread that, every ~1 ms, suspends each other
 * thread of the process, reads its RIP, and resumes it. Every 10 s the
 * histogram (thread id, RIP, hits) is rewritten to <file>, with the module
 * base on the first line, so a run that gets killed still leaves a profile.
 * Resolve offline against the linker map (/MAP): rva = rip - base.
 * HITZ_HOSTPROF_MS sets the sample interval (default 1).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#pragma comment(lib, "winmm.lib")
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HP_SLOTS (1u << 20)
typedef struct { uint64_t rip; uint32_t tid; uint32_t n; } HpSlot;
static HpSlot *s_tab;
static const char *s_path;
static DWORD s_self;
static uint64_t s_exe_lo, s_exe_hi;
static int s_stackwalk = 1, s_heavy_only, s_depth = 1;
extern volatile int g_hitz_heavy;

#define HP_MAXT 256
static struct { DWORD tid; HANDLE h; } s_thr[HP_MAXT];
static int s_nthr;

static void refresh_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    int i;

    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == s_self)
                continue;
            for (i = 0; i < s_nthr && s_thr[i].tid != te.th32ThreadID; i++)
                ;
            if (i < s_nthr || s_nthr == HP_MAXT)
                continue;
            s_thr[s_nthr].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                         THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (s_thr[s_nthr].h) {
                s_thr[s_nthr].tid = te.th32ThreadID;
                s_nthr++;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void record(DWORD tid, uint64_t rip)
{
    uint64_t k = rip * 0x9E3779B97F4A7C15ull ^ tid;
    uint32_t h = (uint32_t)(k >> 40) & (HP_SLOTS - 1), probe;
    for (probe = 0; probe < 64; probe++, h = (h + 1) & (HP_SLOTS - 1)) {
        HpSlot *s = &s_tab[h];
        if (s->n && s->rip == rip && s->tid == tid) { s->n++; return; }
        if (!s->n) { s->rip = rip; s->tid = tid; s->n = 1; return; }
    }
}

static void dump(void)
{
    FILE *f = fopen(s_path, "w");
    uint32_t i;
    if (!f)
        return;
    fprintf(f, "base %llx\n", (unsigned long long)(uintptr_t)GetModuleHandleA(NULL));
    {
        HMODULE mods[512];
        DWORD need = 0, k;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
            for (k = 0; k < need / sizeof(HMODULE) && k < 512; k++) {
                MODULEINFO mi;
                char nm[MAX_PATH];
                if (GetModuleInformation(GetCurrentProcess(), mods[k], &mi, sizeof mi)
                    && GetModuleBaseNameA(GetCurrentProcess(), mods[k], nm, sizeof nm))
                    fprintf(f, "mod %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll,
                            (unsigned long)mi.SizeOfImage, nm);
            }
        }
    }
    for (i = 0; i < HP_SLOTS; i++)
        if (s_tab[i].n)
            fprintf(f, "%u %llx %u\n", s_tab[i].tid, (unsigned long long)s_tab[i].rip, s_tab[i].n);
    fclose(f);
}

static DWORD WINAPI prof_main(LPVOID arg)
{
    DWORD ms = 1, last_refresh = 0, last_dump = GetTickCount();
    const char *e = getenv("HITZ_HOSTPROF_MS");
    (void)arg;
    if (e && atoi(e) > 0)
        ms = (DWORD)atoi(e);
    timeBeginPeriod(1);
    for (;;) {
        DWORD now = GetTickCount();
        int i;
        if (now - last_refresh > 1000) { refresh_threads(); last_refresh = now; }
        for (i = 0; i < s_nthr && (!s_heavy_only || g_hitz_heavy); i++) {
            CONTEXT c;
            if (SuspendThread(s_thr[i].h) == (DWORD)-1)
                continue;
            c.ContextFlags = s_stackwalk ? CONTEXT_FULL : CONTEXT_CONTROL;
            if (GetThreadContext(s_thr[i].h, &c)) {
                uint64_t rip = c.Rip;
                /* Outside the exe (ntdll, driver): attribute to the first exe
                 * return address on the stack, tagged with bit 63. */
                if (s_stackwalk && (rip < s_exe_lo || rip >= s_exe_hi)) {
                    /* Real x64 unwind (system DLLs all carry unwind data) up to
                     * the first frame in the exe; tagged with bit 63. */
                    CONTEXT u = c;
                    int depth, exe_seen = 0;
                    u.ContextFlags = CONTEXT_FULL;
                    for (depth = 0; depth < 48; depth++) {
                        DWORD64 ib = 0, est = 0;
                        PVOID hd = NULL;
                        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
                        if (!fe) {                    /* leaf: return address at [rsp] */
                            MEMORY_BASIC_INFORMATION mb;
                            if (!VirtualQuery((void *)(uintptr_t)u.Rsp, &mb, sizeof mb) || mb.State != MEM_COMMIT) break;
                            u.Rip = *(DWORD64 *)(uintptr_t)u.Rsp;
                            u.Rsp += 8;
                        } else {
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, fe, &u, &hd, &est, NULL);
                        }
                        if (!u.Rip) break;
                        if (u.Rip >= s_exe_lo && u.Rip < s_exe_hi && ++exe_seen >= s_depth) { rip = u.Rip | (1ull << 63); break; }
                    }
                }
                record(s_thr[i].tid, rip);
            }
            ResumeThread(s_thr[i].h);
        }
        if (now - last_dump > 10000) { dump(); last_dump = now; }
        Sleep(ms);
    }
    return 0;
}

void hostprof_start(void)
{
    s_path = getenv("HITZ_HOSTPROF");
    if (!s_path || !*s_path)
        return;
    s_tab = (HpSlot *)calloc(HP_SLOTS, sizeof *s_tab);
    if (getenv("HITZ_HOSTPROF_NOSTACK")) s_stackwalk = 0;
    if (getenv("HITZ_HOSTPROF_HEAVY")) s_heavy_only = 1;
    if (getenv("HITZ_HOSTPROF_DEPTH")) s_depth = atoi(getenv("HITZ_HOSTPROF_DEPTH"));
    if (s_depth < 1) s_depth = 1;
    {
        MODULEINFO mi;
        if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(NULL), &mi, sizeof mi)) {
            s_exe_lo = (uint64_t)(uintptr_t)mi.lpBaseOfDll;
            s_exe_hi = s_exe_lo + mi.SizeOfImage;
        }
    }
    if (!s_tab)
        return;
    {
        HANDLE t = CreateThread(NULL, 0, prof_main, NULL, 0, &s_self);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
            CloseHandle(t);
        }
    }
    fprintf(stderr, "[HITZ] host profiler -> %s\n", s_path);
}
