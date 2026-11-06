/* Built-in cheats of NHL Hitz Pro, set from the launcher.
 *
 * The game has five cheats, unlocked by typing a code as a profile name on
 * the Choose Team screen (HERK, INGY, 211S, SASG, CARB) and then switched
 * under Settings > Game Tuning > Visuals > Cheats. Each one is a setting in
 * the game's current settings block; this sets them (and their unlock bits)
 * from HITZ_CHEAT_* so nobody has to type codes. Unset = the game decides.
 *
 *   HITZ_CHEAT_PLAYER_HEAD  normal | big | huge
 *   HITZ_CHEAT_TEAM_HEAD    normal | big | huge
 *   HITZ_CHEAT_PUCK_SIZE    0 (normal) .. 10   (changes the puck's size AND
 *                           its collision radius: gameplay, not only looks)
 *   HITZ_CHEAT_PUCK_SHADOW  off | on
 *   HITZ_CHEAT_PUCK_GLOW    off | green | yellow | red | blue
 */
#ifndef HITZ_CHEATS_H
#define HITZ_CHEATS_H

#include <stdint.h>

#define HITZ_CHEAT_COUNT 5
#define HITZ_CHEAT_GAME  0xFF          /* not forced: the game's own setting */

/* Read the HITZ_CHEAT_* variables (once; later calls are no-ops). */
void hitz_cheats_init(void);
/* The values in effect, one byte per cheat (HITZ_CHEAT_GAME = not forced). */
void hitz_cheats_get(uint8_t v[HITZ_CHEAT_COUNT]);
/* Replace them (netplay: player 2 takes player 1's). */
void hitz_cheats_set(const uint8_t v[HITZ_CHEAT_COUNT]);
/* Short text for logs, e.g. "player head big, puck glow red". */
const char *hitz_cheats_describe(const uint8_t v[HITZ_CHEAT_COUNT]);
/* Force the cheats into the game's current settings block (guest address).
 * Called from the settings getter (recomp_manual.c), on the game thread. */
void hitz_cheats_apply(uint32_t settings_va);

#endif
