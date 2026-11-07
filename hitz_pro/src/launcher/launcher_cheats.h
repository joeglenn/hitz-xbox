/* The game's built-in cheats in the launcher: a small "Cheats" window
 * (Cheats_ShowDialog), stored in hitz_settings.ini (cheat_* keys) and passed
 * to the game as HITZ_CHEAT_* (see src/cheats.h). "Game setting" leaves a
 * cheat to the game's own Cheats menu.
 *
 * This header is the public interface of launcher_cheats.c for
 * hitz_launcher.c: loading/saving the cheat selections with the rest of the
 * settings ini, building the environment variables for the child game
 * process, and the "Cheats..." button plus its modal-style dialog. All
 * state (the selected index per cheat) is kept inside launcher_cheats.c as
 * module-static data; there is nothing to free or pass a handle for.
 * Single-threaded, called only from the launcher's UI thread. */
#ifndef LAUNCHER_CHEATS_H
#define LAUNCHER_CHEATS_H

#include <windows.h>

/* Number of cheats the game exposes (player heads, team heads, puck size,
 * puck shadow, puck glow); see README.md "Cheats" for the game codes. */
#define CHEATS_ENV_COUNT 5

/* Loads every cheat selection from the ini file at iniPath (cheat_* keys,
 * "game" meaning "leave it to the game's own Cheats menu"). */
void Cheats_Load(LPCWSTR iniPath);
/* Writes every current cheat selection back to the ini file at iniPath. */
void Cheats_Save(LPCWSTR iniPath);
/* Returns TRUE if key is one of the cheat_* ini keys this module owns. */
BOOL Cheats_IsKnownIniKey(LPCWSTR key);
/* Environment entry i (0..CHEATS_ENV_COUNT-1): name, and value when set.
 * Returns FALSE when the variable is to be unset (the game decides). */
BOOL Cheats_Env(int i, LPCWSTR *name, LPWSTR value, size_t cap);
/* Number of cheats currently set to something other than "Game setting". */
int  Cheats_ActiveCount(void);
/* Create the "Cheats..." button (control id `id`) just left of the Play
 * button, which must already exist as control `play_id` of parent. */
HWND Cheats_CreateButton(HWND parent, HINSTANCE inst, int id, int play_id, UINT dpi);
/* Show "Cheats..." / "Cheats (2 on)..." on the button. */
void Cheats_UpdateButton(HWND button);
/* The cheats window, modal over owner. TRUE = the player pressed OK. */
BOOL Cheats_ShowDialog(HWND owner, HFONT font, UINT dpi);

#endif
