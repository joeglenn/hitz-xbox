/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 *
 * Environment variables read in this file: HITZ_DIAG (gates every DIAG
 * wrapper above), HITZ_SYNC_LOAD, HITZ_NO_DEFER, HITZ_KICK_WAIT,
 * RECOMP_UNIMPL_TRAP. Each is documented at its own wrapper/function above.
 *
 * Threading model: every function here runs on whichever guest thread calls
 * the Xbox address it overrides or wraps (the game's main/render thread for
 * most of these; hitz_gpu_swmethod runs on the NV2A pushbuffer executor
 * thread instead, since it is a software-method callback, not a guest call).
 * The deferred pack-load completion queues (s_defer_q and friends) are
 * __declspec(thread): each guest thread that touches packs has its own.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern __declspec(thread) uint32_t g_eax;   /* TLS: see recomp_types.h */
extern ptrdiff_t g_xbox_mem_offset;

/* ── D3D fence wait (D3D_BlockOnTime) ─────────────────────── */

/*
 * NHL Hitz Pro's XDK D3D: device pointer at 0x00269608, fence wait at
 * 0x00254000 (compares device+0x2C with *device+0x30). With RECOMP_PB_EXEC
 * the pushbuffer executor writes BACK_END_WRITE_SEMAPHORE_RELEASE values to
 * the semaphore -- but only once it is told where that is. Hand it the real
 * semaphore (re-checked every call: D3D rewrites it on device reset), then
 * run the original wait, which does its own kickoff first. See
 * xboxrecomp/docs/technical/d3d8ltcg-device-context.md.
 */
#define HITZ_D3D_DEVICE_PTR 0x00269608u

/* D3D's GPU interrupt, the part this title depends on: software method 5
 * (NO_OPERATION 5, which D3D's fence wait plants after WAIT_FOR_IDLE when the
 * GPU is far behind) makes the ISR set the event at device + 0x1DBC, and the
 * wait sleeps on it (KeWaitForSingleObject loop at 0x254150). Without it that
 * wait never ends. Registered from main.c. */
extern int xbox_SignalGuestEvent(uint32_t guest_va);
/* Software-method callback registered with the NV2A pushbuffer executor
 * (nv2a_pb_set_swmethod_callback, from main.c). Runs on the executor thread
 * when it reaches a NO_OPERATION method with a non-zero parameter. Only
 * parameter 5 (D3D's fence-wait notify) does anything: it reads the current
 * D3D device pointer and signals the KEVENT at device+0x1DBC, waking the
 * guest's fence wait. Logs (rate-limited to 4 lines) if the event address is
 * not a KEVENT xbox_SignalGuestEvent recognises, which would mean D3D's
 * device layout assumption above is wrong. */
void hitz_gpu_swmethod(uint32_t param)
{
    static int unknown;
    uint32_t dev;
    if (param != 5)
        return;
    dev = *(volatile uint32_t *)(g_xbox_mem_offset + HITZ_D3D_DEVICE_PTR);
    if (dev && !xbox_SignalGuestEvent(dev + 0x1DBC) && unknown++ < 4)
        fprintf(stderr, "[D3D] notify: event at 0x%08X is not a known KEVENT\n", dev + 0x1DBC);
}
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_00254000_gen(void);

/* Override of the game's D3D fence wait at 0x00254000 (D3D_BlockOnTime,
 * compares device+0x2C with *device+0x30). Before running the original body,
 * reads the GPU-write-back semaphore address out of the live device
 * structure (it can change across a device reset) and, the first time it
 * changes, hands it to the pushbuffer executor via
 * nv2a_pb_set_semaphore_target() and logs it. Without this the executor has
 * nowhere to write its semaphore releases and the fence wait never
 * completes. Runs on the game's D3D/render thread. */
void sub_00254000(void)
{
    static uint32_t s_sem;
    uint32_t dev = *(volatile uint32_t *)(g_xbox_mem_offset + HITZ_D3D_DEVICE_PTR);
    if (dev) {
        uint32_t sem = *(volatile uint32_t *)(g_xbox_mem_offset + dev + 0x30);
        if (sem && sem != s_sem) {
            s_sem = sem;
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n",
                    sem, dev);
        }
    }
    sub_00254000_gen();
}

/* Bring-up diagnostics below run only with HITZ_DIAG=1; otherwise each
 * wrapper goes straight to the generated function. */
static int hitz_diag_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("HITZ_DIAG"); on = e && *e == '1'; }
    return on;
}

/* ── DIAG: mesh draw validator (sub_00015C60) ───────────────── */

/*
 * Temporary. sub_00015C60(this=ecx mesh) walks mesh->[+0x28] materials at
 * [+0x2C] (0x50 bytes each); each material has [+0x48] strips at [+0x4C]
 * (12 bytes: count, ?, index ptr). Boot016 crashed copying indices from
 * 0xC4F6B459. Log the first few meshes with an implausible pointer, with raw
 * dumps, so the bad field can be traced (RECOMP_WATCH) to its writer.
 */
extern __declspec(thread) uint32_t g_ecx, g_esp;
extern void sub_00015C60_gen(void);
#define DIAG_M32(a) (*(volatile uint32_t *)(g_xbox_mem_offset + (uint32_t)(a)))
/* True if p looks like a valid guest pointer: inside the mapped XBE/heap
 * range (0x10000..0x4000000) or the high "cached" alias of it
 * (0x80000000..0x84000000). Used only to decide whether a diagnostic dump is
 * safe to dereference, not as a general validity check. */
static int diag_ptr_ok(uint32_t p)
{
    return (p >= 0x00010000u && p < 0x04000000u) ||
           (p >= 0x80000000u && p < 0x84000000u);
}
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the mesh draw validator at
 * 0x00015C60 (this = ecx = mesh). Before running the generated body, walks
 * the mesh's materials ([+0x28] count, [+0x2C] array of 0x50-byte entries,
 * each with [+0x48] strip count and [+0x4C] strip array of 12-byte entries)
 * and, for the first 6 meshes whose data looks implausible (bad pointers,
 * zero or huge counts), dumps raw header/material/strip words to stderr so
 * the bad field can be traced back to its writer. Read-only: never modifies
 * guest memory, only logs. Runs on whichever thread draws meshes. */
void sub_00015C60(void)
{
    if (!hitz_diag_on()) { sub_00015C60_gen(); return; }
    static int s_logged;
    uint32_t mesh = g_ecx;
    if (s_logged < 6 && diag_ptr_ok(mesh)) {
        uint32_t nmat = DIAG_M32(mesh + 0x28), mats = DIAG_M32(mesh + 0x2C);
        int bad = !diag_ptr_ok(mats) || nmat == 0 || nmat > 256;
        uint32_t m, k, i;
        for (m = 0; !bad && m < nmat; m++) {
            uint32_t mat = mats + m * 0x50;
            uint32_t ns = DIAG_M32(mat + 0x48), st = DIAG_M32(mat + 0x4C);
            if (!diag_ptr_ok(st) || ns == 0 || ns > 4096) { bad = 1; break; }
            for (k = 0; k < ns; k++)
                if (!diag_ptr_ok(DIAG_M32(st + k * 12 + 8))) { bad = 1; break; }
        }
        if (bad) {
            s_logged++;
            fprintf(stderr, "[MESH] bad mesh 0x%08X caller=0x%08X nmat=%u mats=0x%08X\n",
                    mesh, DIAG_M32(g_esp), nmat, mats);
            fprintf(stderr, "[MESH]  hdr:");
            for (i = 0; i < 0x40; i += 4) fprintf(stderr, " %08X", DIAG_M32(mesh + i));
            fprintf(stderr, "\n");
            for (m = 0; diag_ptr_ok(mats) && m < nmat && m < 4; m++) {
                uint32_t mat = mats + m * 0x50;
                uint32_t ns = DIAG_M32(mat + 0x48), st = DIAG_M32(mat + 0x4C);
                fprintf(stderr, "[MESH]  mat%u @0x%08X:", m, mat);
                for (i = 0; i < 0x50; i += 4) fprintf(stderr, " %08X", DIAG_M32(mat + i));
                fprintf(stderr, "\n");
                for (k = 0; diag_ptr_ok(st) && k < ns && k < 4; k++)
                    fprintf(stderr, "[MESH]   strip%u @0x%08X: %08X %08X %08X\n", k,
                            st + k * 12, DIAG_M32(st + k * 12),
                            DIAG_M32(st + k * 12 + 4), DIAG_M32(st + k * 12 + 8));
            }
            fflush(stderr);
        }
    }
    sub_00015C60_gen();
}

/* DIAG: sub_000142C0(prim, count, indices) -> D3D DrawIndexedVerticesUP.
 * Log the first calls with an implausible index pointer: caller + stack. */
extern void sub_000142C0_gen(void);
/* Diagnostic wrapper (HITZ_DIAG=1 only) around the D3D DrawIndexedVerticesUP
 * call at 0x000142C0 (args on the guest stack: prim type, count, index
 * pointer). Logs the first 4 calls whose index pointer fails diag_ptr_ok,
 * with the call's stack arguments and 48 stack words, before letting the
 * generated body run (which would otherwise read indices from a bad
 * pointer, the crash this was added to diagnose). */
void sub_000142C0(void)
{
    if (!hitz_diag_on()) { sub_000142C0_gen(); return; }
    static int s_logged;
    uint32_t idx = DIAG_M32(g_esp + 12);
    if (s_logged < 4 && !diag_ptr_ok(idx)) {
        int i;
        s_logged++;
        fprintf(stderr, "[DRAWIDX] bad indices 0x%08X prim=%u count=%u ret=0x%08X ecx=0x%08X\n",
                idx, DIAG_M32(g_esp + 4), DIAG_M32(g_esp + 8), DIAG_M32(g_esp), g_ecx);
        fprintf(stderr, "[DRAWIDX]  stack:");
        for (i = 0; i < 48; i++) fprintf(stderr, " %08X", DIAG_M32(g_esp + i * 4));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    sub_000142C0_gen();
}

/* DIAG: sub_00016140(this=mesh). From sub_000874C0 the mesh is
 * edi = (*(u32*)0x36E924)[obj->[+8]] with obj = esi. Compare. */
extern __declspec(thread) uint32_t g_esi, g_edi;
extern void sub_00016140_gen(void);
/* Diagnostic wrapper (HITZ_DIAG=1 only) for a mesh accessor at 0x00016140
 * (this = ecx = mesh). Logs the first 6 calls where the mesh pointer looks
 * like a code/rdata address (below 0x00357220, the start of .data) rather
 * than a heap object, together with the caller's object pointer (esi) and
 * what that object's lookup table entry ([0x36E924][obj+8]) resolves to, to
 * compare against the mesh this function actually received. Read-only. */
void sub_00016140(void)
{
    if (!hitz_diag_on()) { sub_00016140_gen(); return; }
    static int s_logged;
    uint32_t mesh = g_ecx, ret = DIAG_M32(g_esp);
    if (s_logged < 6 && mesh < 0x00357220u) {   /* below .data = code/rdata */
        uint32_t obj = g_esi, tab = DIAG_M32(0x36E924u);
        uint32_t id = diag_ptr_ok(obj) ? DIAG_M32(obj + 8) : 0xFFFFFFFFu;
        uint32_t ent = (diag_ptr_ok(tab) && id < 4096) ? DIAG_M32(tab + id * 4) : 0;
        s_logged++;
        fprintf(stderr, "[MESH16140] this=0x%08X ret=0x%08X esi(obj)=0x%08X edi=0x%08X "
                "table=0x%08X id=%u table[id]=0x%08X\n",
                mesh, ret, obj, g_edi, tab, id, ent);
        fflush(stderr);
    }
    sub_00016140_gen();
}

/* DIAG: pack registry. sub_0015DAE0(this=0x584A94 registry, name, ...)
 * starts an async pack load; sub_0015CE60(this, name) looks one up (0 =
 * missing). Log every registration and every failed lookup. */
extern void sub_0015CE60_gen(void);
extern void sub_0015DAE0_gen(void);

/* Copies up to n-1 printable bytes of a NUL-terminated guest string at va
 * into out as a C string, for safe inclusion in a log line; non-printable
 * bytes become '?', and a bad pointer (diag_ptr_ok fails) yields an empty
 * string instead of reading guest memory. Diagnostics-only helper, not used
 * by the game logic. */
static void diag_str(uint32_t va, char *out, int n)
{
    int i;
    for (i = 0; i < n - 1; i++) {
        char c = diag_ptr_ok(va) ? *(volatile char *)(g_xbox_mem_offset + va + i) : 0;
        if (!c) break;
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[i] = 0;
}
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the pack registry's registration
 * call at 0x0015DAE0 (this = registry at 0x584A94, args: name, alias). Logs
 * every registration (name and alias as C strings) before running the
 * generated body, to show what packs the title expects to be able to look
 * up later. */
void sub_0015DAE0(void)
{
    if (!hitz_diag_on()) { sub_0015DAE0_gen(); return; }
    char name[64];
    char alias[64];
    diag_str(DIAG_M32(g_esp + 4), name, sizeof name);
    diag_str(DIAG_M32(g_esp + 8), alias, sizeof alias);
    fprintf(stderr, "[PACK] register \"%s\" as \"%s\" this=0x%08X ret=0x%08X\n",
            name, alias, g_ecx, DIAG_M32(g_esp));
    sub_0015DAE0_gen();
}
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the pack registry's lookup call
 * at 0x0015CE60 (this = registry, arg: name; 0 in eax = not found). Runs the
 * generated body first (so eax holds the real result), then: logs lookup
 * misses (rate-limited to the first 50); for the name "stadanimgeom"
 * specifically, dumps the pack's raw header and its intrusive linked list of
 * geometry objects on a hit, or walks the registry's hash-bucket node chain
 * on a miss; and on a hit for any name seen for the first time, logs how
 * full each of the pack's 11 intrusive lists is. All of this was added to
 * trace why async pack loads came back empty (see the comment above
 * sub_0015C2A0 below). */
void sub_0015CE60(void)
{
    if (!hitz_diag_on()) { sub_0015CE60_gen(); return; }
    char name[64];
    uint32_t nva = DIAG_M32(g_esp + 4), ret = DIAG_M32(g_esp), self = g_ecx;
    diag_str(nva, name, sizeof name);
    sub_0015CE60_gen();
    if (!strcmp(name, "stadanimgeom"))
        fprintf(stderr, "[PACK] lookup \"stadanimgeom\" -> 0x%08X ret=0x%08X flag4e9900=%u\n",
                g_eax, ret, *(volatile uint8_t *)(g_xbox_mem_offset + 0x4E9900u));
    if (!strcmp(name, "stadanimgeom") && diag_ptr_ok(g_eax)) {
        uint32_t pk = g_eax, i, head = pk + 0x58, link = DIAG_M32(head), k = 0;
        fprintf(stderr, "[PACK]  pack raw:");
        for (i = 0; i < 0x74; i += 4) fprintf(stderr, " %08X", DIAG_M32(pk + i));
        fprintf(stderr, "\n");
        while (link && link != head && diag_ptr_ok(link) && k < 12) {
            uint32_t obj = link - 4;
            char nn[48];
            diag_str(DIAG_M32(obj + 0x14), nn, sizeof nn);
            fprintf(stderr, "[PACK]  geom%u obj=0x%08X name@0x%08X \"%s\" raw:", k, obj,
                    DIAG_M32(obj + 0x14), nn);
            for (i = 0; i < 0x40; i += 4) fprintf(stderr, " %08X", DIAG_M32(obj + i));
            fprintf(stderr, "\n");
            link = DIAG_M32(obj + 4);
            k++;
        }
        fprintf(stderr, "[PACK]  list end link=0x%08X after %u\n", link, k);
    }
    if (g_eax && diag_ptr_ok(g_eax)) {
        /* First successful lookup of each name: how full are its 11 lists
         * (heads at +0x08..+0x58, 8 bytes apart). */
        static uint32_t seen[256];
        static int nseen;
        uint32_t pk = g_eax, h, i;
        int j, dup = 0;
        for (j = 0; j < nseen; j++) if (seen[j] == pk) dup = 1;
        if (!dup && nseen < 256) {
            seen[nseen++] = pk;
            fprintf(stderr, "[PACKFILL] \"%s\" pack=0x%08X lists:", name, pk);
            for (h = 0x08; h <= 0x58; h += 8) {
                uint32_t head = pk + h, l = DIAG_M32(head), c = 0;
                while (l != head && diag_ptr_ok(l) && c < 100000) { l = DIAG_M32(l); c++; }
                fprintf(stderr, " %u", c);
            }
            fprintf(stderr, " | +5C..: ");
            for (i = 0x5C; i < 0x74; i += 4) fprintf(stderr, " %08X", DIAG_M32(pk + i));
            fprintf(stderr, "\n");
        }
    }
    if (!g_eax) {
        static int n;
        if (n++ < 50)
            fprintf(stderr, "[PACK] lookup MISS \"%s\" this=0x%08X ret=0x%08X\n",
                    name, self, ret);
        if (!strcmp(name, "stadanimgeom")) {
            uint32_t head = self + 0x10, node = DIAG_M32(head), k = 0;
            while (node != head && diag_ptr_ok(node) && k < 200) {
                char nn[48];
                diag_str(node + 8, nn, sizeof nn);
                fprintf(stderr, "[PACK]   node%u 0x%08X hash=0x%08X +8\"%s\" raw:", k, node,
                        DIAG_M32(node + 0x64), nn);
                {
                    uint32_t i;
                    for (i = 0; i < 0x74; i += 4) fprintf(stderr, " %08X", DIAG_M32(node + i));
                }
                fprintf(stderr, "\n");
                node = DIAG_M32(node);
                k++;
            }
            fprintf(stderr, "[PACK]   %u nodes\n", k);
        }
    }
}

/* EXPERIMENT: HITZ_SYNC_LOAD=1 clears the async-load flag (byte 0x37B5CD)
 * before every pack load request (sub_0015DA40), forcing the synchronous
 * path. Tells a broken async loader from a broken parser. */
extern void sub_0015DA40_gen(void);
/* Override of the pack-load-request function at 0x0015DA40. When
 * HITZ_SYNC_LOAD=1, clears the async-load flag byte at guest address
 * 0x37B5CD before every call, forcing the title's synchronous load path
 * instead of the asynchronous one; used (A/B) to tell a broken async loader
 * from a broken parser. Off by default, in which case this is a pure
 * pass-through to the generated body. */
void sub_0015DA40(void)
{
    static int s_sync = -1;
    if (s_sync < 0) {
        const char *e = getenv("HITZ_SYNC_LOAD");
        s_sync = (e && *e == '1');
        if (s_sync) fprintf(stderr, "[HITZ] synchronous pack loading forced\n");
    }
    if (s_sync)
        *(volatile uint8_t *)(g_xbox_mem_offset + 0x37B5CDu) = 0;
    sub_0015DA40_gen();
}

/* ── FIX: async pack-load completion ordering (sub_0015C2A0 / sub_0015C3D0) ── */

/*
 * sub_0015C2A0 opens a pack file, allocates its buffer (job+0x218), queues
 * the read on the game's I/O layer (sub_001CA200, completion callback
 * sub_0015C3D0(fileobj, job)), and only AFTER that call returns writes the
 * buffer header pointer job+0x21C (0xFFFFFFFF marker + size) that the
 * completion's parser (sub_0015C120 -> sub_0015CDD0) reads.
 *
 * On the console the DVD read is pending when sub_001CA200 returns, so the
 * completion always runs later. Here NtReadFile completes synchronously and
 * the game's I/O state machine dispatches the completion INSIDE the submit
 * call, on the same thread: the parser got data=8/hdr=0 and every async pack
 * (.xbx geometry, anims) came out empty -> black 3D, then a crash on a
 * never-filled model table (sub_000874C0 / table at [0x36E924]).
 *
 * Restore the console's ordering: a completion that arrives while its thread
 * is inside sub_0015C2A0 is queued and run as soon as the outermost
 * sub_0015C2A0 returns. HITZ_NO_DEFER=1 disables (A/B testing).
 */
/* (WIN32_LEAN_AND_MEAN set at the top) */
#include <windows.h>
extern void sub_0015C2A0_gen(void);
extern void sub_0015C3D0_gen(void);
static __declspec(thread) int s_c2a0_depth;
static __declspec(thread) uint32_t s_defer_q[128][2];
static __declspec(thread) int s_defer_head, s_defer_tail;
/* Reads HITZ_NO_DEFER once (cached) and logs whether deferred pack-load
 * completions are enabled; returns true (deferral on, the default) unless
 * HITZ_NO_DEFER=1. */
static int defer_enabled(void)
{
    static int en = -1;
    if (en < 0) {
        const char *e = getenv("HITZ_NO_DEFER");
        en = !(e && *e == '1');
        fprintf(stderr, "[HITZ] deferred pack-load completions %s\n", en ? "on" : "OFF");
    }
    return en;
}
/* Runs the generated pack-load completion body (0x0015C3D0) for one queued
 * (a1, a2) entry, by hand-building the cdecl call frame it expects on the
 * guest stack: push a2, push a1, push a dummy return address, call, then pop
 * the two arguments back off after it returns (the body's own `ret` already
 * consumed the dummy return slot). Called only from defer_leave(), on the
 * thread that is draining the deferred queue. */
static void run_completion(uint32_t a1, uint32_t a2)
{
    /* cdecl call into the generated body: push a2, a1, dummy return. The
     * body's `ret` pops the return slot; the caller cleans the two args. */
    g_esp -= 12;
    DIAG_M32(g_esp) = 0;
    DIAG_M32(g_esp + 4) = a1;
    DIAG_M32(g_esp + 8) = a2;
    sub_0015C3D0_gen();
    g_esp += 8;
}
/* Pairs with entering the outer pack-load call (0x0015C2A0 or 0x0002A4C0):
 * decrements the thread-local nesting depth, and once it reaches 0 (the
 * outermost call is returning) drains every completion queued while that
 * call was in progress, in order, running each through run_completion().
 * Draining a completion can itself queue more (re-entering the outer call
 * for the next job), which the while loop picks up. */
static void defer_leave(void)
{
    s_c2a0_depth--;
    if (s_c2a0_depth == 0) {
        while (s_defer_head != s_defer_tail) {
            uint32_t a1 = s_defer_q[s_defer_head][0], a2 = s_defer_q[s_defer_head][1];
            s_defer_head = (s_defer_head + 1) & 127;
            run_completion(a1, a2);   /* may re-enter sub_0015C2A0 (next job) */
        }
    }
}
/* Override of the pack-open function at 0x0015C2A0 (opens a file, queues its
 * read, and on the console would still be pending when this returns).
 * Brackets the generated body with the deferral nesting counter, so any
 * read completion that fires synchronously inside it (because the host's
 * ReadFile completed before this call returned, unlike on a real DVD drive)
 * is queued by sub_0015C3D0 instead of running immediately, and runs only
 * once this (possibly nested) call fully unwinds. See the comment above
 * the deferral machinery for why: without it, async packs came back empty. */
void sub_0015C2A0(void)
{
    s_c2a0_depth++;
    sub_0015C2A0_gen();
    defer_leave();
}
/*
 * sub_0002A4C0 is the front-end/match-load message handler. Its loading
 * states queue a batch of packs through sub_00028AC0, which bumps a pending
 * counter (0x3FCDFC); each pack's completion (sub_00028A70) decrements it and
 * posts message 0x117 ("batch done -> load the crowd") on reaching 0. On the
 * console every pack in the batch is queued before the first DVD read ends,
 * so 0x117 fires once. Completing each pack before the handler queues the
 * next one made the counter hit 0 after EVERY pack: NewCrowd.pck was loaded
 * ~20 times, its textures exhausted the 64 MB contiguous arena and the game
 * crashed on the failed allocation. Hold completions until the handler
 * returns, like the console's in-flight reads.
 */
extern void sub_0002A4C0_gen(void);
/* Override of the front-end/match-load message handler at 0x0002A4C0. Same
 * deferral bracket as sub_0015C2A0, for the same reason: its loading states
 * queue a batch of packs whose completions must not be allowed to drain the
 * pending counter to 0 (and fire the "batch done" message) until every pack
 * in the batch has actually been queued, as happens naturally on the
 * console's own asynchronous DVD reads. A no-op pass-through when deferral
 * is disabled (HITZ_NO_DEFER=1). */
void sub_0002A4C0(void)
{
    if (!defer_enabled()) { sub_0002A4C0_gen(); return; }
    s_c2a0_depth++;
    sub_0002A4C0_gen();
    defer_leave();
}
/* Override of the pack-load completion callback at 0x0015C3D0(fileobj, job).
 * While deferral is enabled and the calling thread is inside a bracketed
 * outer call (s_c2a0_depth > 0), queues (a1, a2) into the thread-local ring
 * s_defer_q instead of running the generated body now, mimicking the
 * guest stack's `ret` (esp += 4) so the caller sees a normal return; falls
 * back to running inline (with a warning) if the queue is full. Runs the
 * generated body directly whenever deferral does not apply (disabled, or
 * called from outside any bracketed outer call). */
void sub_0015C3D0(void)
{
    if (defer_enabled() && s_c2a0_depth > 0) {
        int nt = (s_defer_tail + 1) & 127;
        if (nt != s_defer_head) {
            s_defer_q[s_defer_tail][0] = DIAG_M32(g_esp + 4);
            s_defer_q[s_defer_tail][1] = DIAG_M32(g_esp + 8);
            s_defer_tail = nt;
            g_esp += 4;          /* behave like the body's `ret` */
            return;
        }
        fprintf(stderr, "[HITZ] deferred completion queue full; running inline\n");
    }
    sub_0015C3D0_gen();
}

/* DIAG: pack parser sub_0015CDD0(this=registry, data, hdr). */
extern void sub_0015CDD0_gen(void);
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the pack parser at 0x0015CDD0
 * (this = registry, args: data buffer, header). Logs the first 60 calls
 * with this/data/hdr and the first few words of each buffer, plus the
 * calling thread id, before running the generated body; part of tracing why
 * async-loaded pack data was seen as empty (data=8/hdr=0). */
void sub_0015CDD0(void)
{
    if (!hitz_diag_on()) { sub_0015CDD0_gen(); return; }
    static int n;
    uint32_t a1 = DIAG_M32(g_esp + 4), a2 = DIAG_M32(g_esp + 8), ret = DIAG_M32(g_esp);
    if (n++ < 60)
        fprintf(stderr, "[PARSE] this=0x%08X data=0x%08X hdr=0x%08X ret=0x%08X data[0..3]=%08X %08X %08X %08X"
                " hdr[0..1]=%08X %08X tid=%lu\n", g_ecx, a1, a2, ret,
                diag_ptr_ok(a1) ? DIAG_M32(a1) : 0, diag_ptr_ok(a1) ? DIAG_M32(a1 + 4) : 0,
                diag_ptr_ok(a1) ? DIAG_M32(a1 + 8) : 0, diag_ptr_ok(a1) ? DIAG_M32(a1 + 12) : 0,
                diag_ptr_ok(a2) ? DIAG_M32(a2) : 0, diag_ptr_ok(a2) ? DIAG_M32(a2 + 4) : 0,
                (unsigned long)GetCurrentThreadId());
    sub_0015CDD0_gen();
}

/* DIAG: sub_0015C120 (async parse step, job in esi). */
extern __declspec(thread) uint32_t g_ebx;
extern void sub_0015C120_gen(void);
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the async pack parse step at
 * 0x0015C120 (job pointer in esi). Logs the first 40 calls with the job's
 * name string and its +0x218/+0x21C/+0x208 header fields (the buffer
 * pointer, the 0xFFFFFFFF-marker-plus-size header, and a third field),
 * which is the data the completion-ordering bug above (sub_0015C2A0) made
 * arrive as zeroes. */
void sub_0015C120(void)
{
    if (!hitz_diag_on()) { sub_0015C120_gen(); return; }
    static int n;
    if (n++ < 40) {
        char nm[64];
        uint32_t job = g_esi;
        diag_str(job, nm, sizeof nm);
        fprintf(stderr, "[C120] esi(job)=0x%08X \"%s\" +218=0x%08X +21C=0x%08X +208=0x%08X ret=0x%08X edi=0x%08X ebx=0x%08X\n",
                job, nm, diag_ptr_ok(job) ? DIAG_M32(job + 0x218) : 0,
                diag_ptr_ok(job) ? DIAG_M32(job + 0x21C) : 0,
                diag_ptr_ok(job) ? DIAG_M32(job + 0x208) : 0, DIAG_M32(g_esp), g_edi, g_ebx);
    }
    sub_0015C120_gen();
}

/* DIAG: game message post sub_000C7270(this, msg) and load-counter callback. */
extern void sub_000C7270_gen(void);
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the game message post function
 * at 0x000C7270 (this, msg). Logs only messages 0x116 (crowd pack done) and
 * 0x117 (batch done -> load the crowd), with the pending-load counter at
 * 0x3FCDFC and flag byte at 0x4E7654, rate-limited to the first 200; added
 * to trace the crowd-loaded-20-times bug. */
void sub_000C7270(void)
{
    if (!hitz_diag_on()) { sub_000C7270_gen(); return; }
    uint32_t msg = DIAG_M32(g_esp + 4), ret = DIAG_M32(g_esp);
    if (msg == 0x116 || msg == 0x117) {
        static int n;
        if (n++ < 200)
            fprintf(stderr, "[MSG] post 0x%X ret=0x%08X ret2=0x%08X cnt3fcdfc=%d flag4e7654=%d\n", msg, ret,
                    DIAG_M32(g_esp + 8), (int)DIAG_M32(0x3FCDFCu), (int)DIAG_M32(0x4E7654u));
    }
    sub_000C7270_gen();
}
extern void sub_00028A70_gen(void);
/* Diagnostic wrapper (HITZ_DIAG=1 only) for the per-pack load-completion
 * callback at 0x00028A70, which decrements the pending-batch counter at
 * 0x3FCDFC. Logs the counter's value before and after the decrement
 * (rate-limited to the first 400 calls), before running the generated body
 * that actually decrements it. */
void sub_00028A70(void)
{
    if (!hitz_diag_on()) { sub_00028A70_gen(); return; }
    static int n;
    if (n++ < 400)
        fprintf(stderr, "[LOADCNT] dec %d -> %d ret=0x%08X\n", (int)DIAG_M32(0x3FCDFCu),
                (int)DIAG_M32(0x3FCDFCu) - 1, DIAG_M32(g_esp));
    sub_00028A70_gen();
}

/* D3D KickOff: PFB flush completes at once (see src/recomp_kick.c). */
extern void sub_00253E30_gen(void);
extern void hitz_kick_impl(void);
/* Override of D3D's KickOff at 0x00253E30. Normally runs hitz_kick_impl()
 * (src/recomp_kick.c), the same logic with the PFB-flush wait replaced by an
 * immediate completion; HITZ_KICK_WAIT=1 instead runs the untouched
 * generated body, which spins until the executor thread clears the flush
 * bit, for A/B comparison. The choice is read from the environment once and
 * cached, with a one-line log of which path is active. */
void sub_00253E30(void)
{
    static int wait = -1;
    if (wait < 0) {
        const char *e = getenv("HITZ_KICK_WAIT");
        wait = e && *e == '1';
        fprintf(stderr, "[HITZ] D3D kickoff flush wait %s\n", wait ? "ON (original)" : "off");
    }
    if (wait) sub_00253E30_gen(); else hitz_kick_impl();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Built-in cheats from the launcher (cheats.c) ───────────────────────
 * 0x999B0 returns the settings block of the object in ecx (ecx + 0x110).
 * For the game's settings manager (MEM32(0x4EB8E4)) that is the current
 * settings, read by the menus and the match set-up: force the launcher's
 * cheats into it on the way out. Nothing changes when no cheat is set. */
#include "cheats.h"
extern void sub_000999B0_gen(void);

/* Override of the settings-block getter at 0x000999B0 (this = ecx). Runs
 * the generated body first (eax = the returned settings block pointer),
 * then, only when the receiver is the game's settings manager singleton
 * (MEM32(0x4EB8E4)) and not some other caller of the same getter, applies
 * the launcher's forced cheat values into that block via
 * hitz_cheats_apply(). A no-op whenever no cheat is forced. */
void sub_000999B0(void)
{
    uint32_t self = g_ecx;
    sub_000999B0_gen();
    if (self && self == *(volatile uint32_t *)(g_xbox_mem_offset + 0x4EB8E4u))
        hitz_cheats_apply(g_eax);
}
