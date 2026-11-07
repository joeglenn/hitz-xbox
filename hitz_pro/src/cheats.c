/* Built-in cheats from the launcher: see cheats.h.
 *
 * Found in the game (addresses of the USA Xbox build):
 * - The current settings block is MEM32(MEM32(0x4EB8E4) + 0x110), 0x438
 *   bytes; profiles are copied into it (0x999C0) and the getter 0x999B0
 *   returns it. Cheat settings by menu id (getter 0xF7040, setter 0xF7440):
 *   0x33 player head +0x3E0, 0x34 team head +0x3E8, 0x35 puck size +0x3F8,
 *   0x36 puck shadow +0x40C, 0x38 puck glow +0x3F4.
 * - +0x434 is the unlock mask: bit 0 HERK (player head), 1 INGY (team
 *   head), 2 211S (puck size), 3 SASG (puck shadow), 4 CARB (puck glow)
 *   (pkChooseTeams 0xE50D0; the Cheats menu 0xF8A13 greys out the rest).
 * - The puck size setter also stores the size as a float at 0x4E79C8; the
 *   match set-up (0xBC750) turns it into the puck's scale, 1 + 0.15 * size,
 *   which sizes its model and its collision radius.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"

extern ptrdiff_t g_xbox_mem_offset;

/* Guest-address accessors: g_xbox_mem_offset is added to a 32-bit Xbox
 * virtual address to get the matching host pointer into the mapped guest
 * memory (see xbox_memory_layout.c). volatile because the same guest word
 * can be written by guest code running concurrently. */
#define GUEST32(va) (*(volatile uint32_t *)(g_xbox_mem_offset + (uint32_t)(va)))
#define GUESTF(va)  (*(volatile float *)(g_xbox_mem_offset + (uint32_t)(va)))

/* Offset, within the current settings block, of the 32-bit unlock-bit mask
 * (bit per cheat, see k_cheat[].unlock_bit). Guest address, not a byte count
 * from this file's start. */
#define SETTINGS_OFF_MASK 0x434u
/* Guest address (bss) of the puck-size float the match setup reads to scale
 * the puck model and its collision radius: scale = 1 + 0.15 * size. */
#define PUCK_SIZE_FLOAT   0x4E79C8u

/* One row per cheat, in cheats.h's HITZ_CHEAT_* order. env/name are for
 * logging and parsing; off is the cheat's byte offset in the settings block;
 * unlock_bit is its bit in the mask at SETTINGS_OFF_MASK; max is the highest
 * valid stored value (0..max); values[] are the words parse() accepts for
 * each stored value (puck size has no words, only numbers). */
static const struct {
    const char *env;
    const char *name;
    uint32_t    off;          /* in the settings block */
    uint32_t    unlock_bit;
    uint8_t     max;          /* values 0..max */
    const char *values[11];   /* env words; NULL entries = numbers only */
} k_cheat[HITZ_CHEAT_COUNT] = {
    { "HITZ_CHEAT_PLAYER_HEAD", "player head", 0x3E0, 1u << 0, 2,  { "normal", "big", "huge" } },
    { "HITZ_CHEAT_TEAM_HEAD",   "team head",   0x3E8, 1u << 1, 2,  { "normal", "big", "huge" } },
    { "HITZ_CHEAT_PUCK_SIZE",   "puck size",   0x3F8, 1u << 2, 10, { NULL } },
    { "HITZ_CHEAT_PUCK_SHADOW", "puck shadow", 0x40C, 1u << 3, 1,  { "off", "on" } },
    { "HITZ_CHEAT_PUCK_GLOW",   "puck glow",   0x3F4, 1u << 4, 4,  { "off", "green", "yellow", "red", "blue" } },
};

static int     s_init;
static uint8_t s_v[HITZ_CHEAT_COUNT];
static int     s_any;               /* any cheat forced: the fast path skips */

/* Recomputes s_any from s_v[]; called after anything changes s_v[]. */
static void recount(void)
{
    int i;
    s_any = 0;
    for (i = 0; i < HITZ_CHEAT_COUNT; i++)
        if (s_v[i] != HITZ_CHEAT_GAME) s_any = 1;
}

/* Turns one HITZ_CHEAT_* environment string into the stored byte value for
 * cheat index i: a word match against k_cheat[i].values, a plain number
 * within range, "game"/empty/unset for HITZ_CHEAT_GAME, or (with a warning
 * to stderr) HITZ_CHEAT_GAME for anything else it cannot parse. */
static uint8_t parse(int i, const char *e)
{
    int j;
    char *end;
    long n;
    if (!e || !*e || !_stricmp(e, "game")) return HITZ_CHEAT_GAME;
    for (j = 0; j <= k_cheat[i].max; j++)
        if (k_cheat[i].values[j] && !_stricmp(e, k_cheat[i].values[j])) return (uint8_t)j;
    n = strtol(e, &end, 10);
    if (*end == 0 && n >= 0 && n <= k_cheat[i].max) return (uint8_t)n;
    fprintf(stderr, "[CHEAT] %s=%s is not a value it takes; left to the game\n", k_cheat[i].env, e);
    return HITZ_CHEAT_GAME;
}

/* Reads every HITZ_CHEAT_* variable into s_v[] and logs the result if any
 * cheat is forced. Idempotent: s_init makes every call after the first a
 * no-op, so callers do not need to coordinate who calls it first. */
void hitz_cheats_init(void)
{
    int i;
    if (s_init) return;
    s_init = 1;
    for (i = 0; i < HITZ_CHEAT_COUNT; i++) s_v[i] = parse(i, getenv(k_cheat[i].env));
    recount();
    if (s_any) fprintf(stderr, "[CHEAT] %s\n", hitz_cheats_describe(s_v));
}

/* Copies the current cheat values out to the caller, parsing the environment
 * first if that has not happened yet. */
void hitz_cheats_get(uint8_t v[HITZ_CHEAT_COUNT])
{
    hitz_cheats_init();
    memcpy(v, s_v, sizeof s_v);
}

/* Replaces the cheat values wholesale, e.g. with the host's values received
 * over netplay. Out-of-range entries fall back to HITZ_CHEAT_GAME rather
 * than being stored as-is. Marks the module initialised, so a later
 * hitz_cheats_init()/_get() does not overwrite these with the environment. */
void hitz_cheats_set(const uint8_t v[HITZ_CHEAT_COUNT])
{
    int i;
    s_init = 1;
    for (i = 0; i < HITZ_CHEAT_COUNT; i++)
        s_v[i] = (v[i] == HITZ_CHEAT_GAME || v[i] <= k_cheat[i].max) ? v[i] : HITZ_CHEAT_GAME;
    recount();
}

/* Formats the non-"game" entries of v as a short comma-separated line for
 * logging, e.g. "player head big, puck glow red". Returns a pointer to a
 * static buffer (not reentrant/thread-safe); returns "no cheats" if every
 * entry is HITZ_CHEAT_GAME. Truncates silently if the buffer is too small
 * for all five entries. */
const char *hitz_cheats_describe(const uint8_t v[HITZ_CHEAT_COUNT])
{
    static char buf[160];
    size_t o = 0;
    int i;
    buf[0] = 0;
    for (i = 0; i < HITZ_CHEAT_COUNT; i++) {
        char val[12];
        if (v[i] == HITZ_CHEAT_GAME) continue;
        if (v[i] <= k_cheat[i].max && k_cheat[i].values[v[i]]) snprintf(val, sizeof val, "%s", k_cheat[i].values[v[i]]);
        else snprintf(val, sizeof val, "%u", v[i]);
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%s %s", o ? ", " : "", k_cheat[i].name, val);
        if (o >= sizeof buf) break;
    }
    return o ? buf : "no cheats";
}

/* Writes the forced cheat values (and their unlock bits) into the game's
 * current settings block at guest address settings_va, and mirrors a forced
 * puck size into the float the match setup reads for puck scale. A no-op if
 * no cheat is forced (s_any) or if settings_va is outside the plausible
 * guest address range (0x10000..0x4000000), which guards against being
 * called before the settings block exists. Called from the settings getter
 * override in recomp_manual.c, so this runs on the game thread, synchronously
 * with whichever guest code just asked for its settings. */
void hitz_cheats_apply(uint32_t settings_va)
{
    int i;
    uint32_t mask;
    if (!s_init) hitz_cheats_init();
    if (!s_any || settings_va < 0x10000u || settings_va >= 0x4000000u) return;
    mask = GUEST32(settings_va + SETTINGS_OFF_MASK);
    for (i = 0; i < HITZ_CHEAT_COUNT; i++) {
        if (s_v[i] == HITZ_CHEAT_GAME) continue;
        GUEST32(settings_va + k_cheat[i].off) = s_v[i];
        mask |= k_cheat[i].unlock_bit;
    }
    GUEST32(settings_va + SETTINGS_OFF_MASK) = mask;
    if (s_v[2] != HITZ_CHEAT_GAME) GUESTF(PUCK_SIZE_FLOAT) = (float)s_v[2];
}
