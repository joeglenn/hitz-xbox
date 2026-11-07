/*
 * recomp_kick.c -- D3D KickOff (sub_00253E30) without the PFB-flush wait.
 * Generated body of sub_00253E30 (recomp_0065.c), copied with the flush
 * request/spin replaced by an immediate completion. Called from the
 * sub_00253E30 override in recomp_manual.c. HITZ_KICK_WAIT=1 there runs the
 * original instead (A/B).
 * Regenerate this file if the recompiler's output for sub_00253E30 changes.
 *
 * No environment variables are read directly in this file; HITZ_KICK_WAIT is
 * read by the caller in recomp_manual.c, which decides whether this function
 * or the generated original runs. Runs on whichever guest thread calls the
 * D3D KickOff wrapper (normally the game's main/render thread).
 */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"
#include <math.h>
#include <intrin.h>

/* Lifted body of the guest function at 0x00253E30 (D3D's KickOff), labels
 * and all, except that the PFB write-buffer flush (device+0x100410 bit
 * 0x10000) is cleared immediately below instead of being polled until the
 * executor thread clears it, and an _mm_mfence replaces that spin to keep
 * pushbuffer writes ordered before PUT is published. Side effects: writes
 * the device's PFB flush register and the pushbuffer PUT pointer in guest
 * memory (ecx/esi fields at the offsets named in the labels below); no
 * return value (void), matching the original's register-based ABI (uses the
 * global guest registers ecx/eax/edx/esi and the guest stack via
 * PUSH32/POP32). Falls straight through to RET; never fails. */
void hitz_kick_impl(void)
{
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;

loc_00253E30: ;
    PUSH32(esp, esi);
    esi = ecx;
    eax = MEM32(esi + 8);
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(4) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test LO8(eax), 4 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_00253E42; /* je: equal / zero */

loc_00253E3A: ;
    edx = MEM32(esi + 0x76C);
    goto loc_00253E44;

loc_00253E42: ;
    edx = MEM32(esi);

loc_00253E44: ;
    _fa = (uint32_t)(HI8(eax)) & 0xFFu; _fb = (uint32_t)(0x20) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test HI8(eax), 0x20 (8-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_00253EA6; /* jne: not equal / not zero */

loc_00253E49: ;
    eax = MEM32(0x269608);
    eax = MEM32(eax + 0x1C18);
    (void)0; /* sfence: cache/ordering hint, nothing to model */
    ecx = MEM32(eax + 0x100410);
    /* HITZ: the PFB flush completes at once. On hardware it is a write-buffer
     * flush (microseconds), independent of GPU progress; here every kick
     * waited ~5 ms for the executor thread to clear the bit -- 150 kicks a
     * frame turned the 3D scene into 10 fps. Writes are already ordered. */
    ecx = ecx & ~0x10000u;
    MEM32(eax + 0x100410) = ecx;
    /* nop */

    /* (flush wait removed: the bit is never left set) */

loc_00253E7C: ;
    ecx = MEM32(esi + 0x1C10);
    edx = edx & 0xFFFFFFF;
    _fa = (uint32_t)(edx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* and result */
    _ReadWriteBarrier(); _mm_mfence();   /* pushbuffer words visible before PUT (was the flush spin) */
    MEM32(ecx + 0x40) = edx;
    eax = MEM32(0x269160);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00253EDE; /* je: equal / zero */

loc_00253E94: ;
    PUSH32(esp, 0x00253E99u); RECOMP_ABI_CALL(0x0024BAD0u, sub_0024BAD0); /* call 0x0024BAD0 */

loc_00253E99: ;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_00253E94; /* jne: not equal / not zero */

loc_00253E9D: ;
    MEM32(esi + 8) = MEM32(esi + 8) | 0x2000;
    _fa = (uint32_t)(MEM32(esi + 8)) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* or result */
    POP32(esp, esi);
    esp += 4; return; /* ret */

loc_00253EA6: ;
    eax = MEM32(esi);
    ecx = 0x269150;
    _fb = (uint32_t)(0x40) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* sub source, before the write */
    ecx = ecx - 0x40;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* sub result */
    MEM32(esi + 0x1C10) = ecx;
    eax = eax & 0xFFFFFFF;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* and result */
    MEM32(ecx + 0x40) = eax;
    edx = MEM32(esi + 0x1C10);
    MEM32(edx + 0x44) = eax;
    eax = MEM32(esi + 0x2C);
    ecx = MEM32(esi + 0x30);
    _fb = (uint32_t)(2) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* sub source, before the write */
    eax = eax - 2;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* sub result */
    MEM32(ecx) = eax;
    edx = MEM32(esi + 0x2468);
    MEM32(esi + 0x1DD4) = edx;

loc_00253EDE: ;
    POP32(esp, esi);
    esp += 4; return; /* ret */

}
