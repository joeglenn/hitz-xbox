/*
 * hostprof_win.c -- in-process sampling profiler for the Windows build.
 *
 * HITZ_HOSTPROF=<file> starts a thread that, every ~1 ms, suspends each other
 * thread of the process, reads its RIP, and resumes it. Every 10 s the
 * histogram (thread id, RIP, hits) is rewritten to <file>, with the module
 * base on the first line, so a run that gets killed still leaves a profile.
 * Resolve offline against the linker map (/MAP): rva = rip - base.
 * HITZ_HOSTPROF_MS sets the sample interval (default 1).
 *
 * Other environment variables, read once in hostprof_start():
 *   HITZ_HOSTPROF_NOSTACK - disable the call-stack walk for RIPs outside
 *                      the exe (s_stackwalk); samples there are then
 *                      recorded as-is instead of attributed to an exe frame.
 *   HITZ_HOSTPROF_HEAVY - only sample while g_hitz_heavy is set (defined
 *                      elsewhere in the project), to profile just a known
 *                      heavy phase.
 *   HITZ_HOSTPROF_DEPTH - how many exe-side call frames to skip before
 *                      recording one, when walking out of a non-exe RIP
 *                      (s_depth, minimum 1); lets a known wrapper function
 *                      be skipped so time is attributed to its caller.
 *
 * Threading model: hostprof_start() runs on the caller's thread (main.c's
 * WinMain) and only starts the profiler thread (prof_main); all sampling,
 * stack walking and file output happen on that one profiler thread, which
 * suspends/resumes the other threads of the process around reading their
 * context and runs until the process exits.
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
/* One histogram entry: a (thread id, RIP) pair seen at least once, and how
 * many samples hit it. n == 0 marks an unused slot (see record()). */
typedef struct { uint64_t rip; uint32_t tid; uint32_t n; } HpSlot;
static HpSlot *s_tab;          /* HP_SLOTS-entry open-addressed hash table */
static const char *s_path;     /* HITZ_HOSTPROF output file path */
static DWORD s_self;           /* profiler thread's own id, never sampled */
static uint64_t s_exe_lo, s_exe_hi;  /* [lo, hi) address range of this exe */
static int s_stackwalk = 1, s_heavy_only, s_depth = 1;
extern volatile int g_hitz_heavy;

#define HP_MAXT 256
static struct { DWORD tid; HANDLE h; } s_thr[HP_MAXT];
static int s_nthr;

/* Rescans the process's threads and opens a handle (with suspend/resume and
 * get-context rights) for every one not already tracked, up to HP_MAXT.
 * Called about once a second from prof_main, not on every sample, since
 * enumerating threads is comparatively expensive. Threads that exit are not
 * removed (their handle just stops showing real activity); failures to
 * snapshot or open a thread are silently skipped. */
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

/* Adds one sample (thread id, instruction pointer) to the open-addressed
 * histogram s_tab. Hashes (rip, tid) and linearly probes up to 64 slots
 * looking for a matching entry to increment or an empty slot to claim; a
 * sample that cannot find either within 64 probes is dropped (silently: the
 * table is large enough in practice that this is not expected). Called from
 * the profiler thread only. */
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

/* Rewrites s_path with the full current histogram: the exe's load base, the
 * base/size/name of every loaded module (for offline RVA resolution), then
 * one line per non-empty histogram slot (tid, rip, hit count). Called every
 * 10 s from the profiler thread so a killed or crashed run still leaves a
 * usable profile; silently does nothing if s_path cannot be opened. */
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

/* The profiler thread body: refreshes the thread list about once a second,
 * then every sample interval suspends each tracked thread (skipping
 * suspend/resume for ones outside the heavy phase when HITZ_HOSTPROF_HEAVY
 * is set), reads its RIP, and resumes it. A RIP outside the exe is walked
 * up the real call stack (RtlVirtualUnwind, falling back to a plain [rsp]
 * read for frames with no unwind data) until a frame inside the exe is seen
 * (skipping s_depth-1 of them, so HITZ_HOSTPROF_DEPTH can look further up a
 * known wrapper), tagged with the top bit so dump()'s consumer can tell an
 * attributed wait from a direct sample. Writes the histogram file every
 * 10 s. Runs forever (the process exit ends it); never returns normally. */
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

/* Entry point called once from main.c. A no-op unless HITZ_HOSTPROF names an
 * output file: then it allocates the histogram, reads the HITZ_HOSTPROF_*
 * tuning variables, records the exe's address range (for the exe/non-exe
 * split in prof_main), and starts the profiler thread at time-critical
 * priority so sampling is not starved by the game itself. Leaves the
 * profiler disabled (and returns early) if the histogram allocation fails. */
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
