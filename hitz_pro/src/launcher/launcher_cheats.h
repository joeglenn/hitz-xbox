/* The game's built-in cheats in the launcher: a small "Cheats" window
 * (Cheats_ShowDialog), stored in hitz_settings.ini (cheat_* keys) and passed
 * to the game as HITZ_CHEAT_* (see src/cheats.h). "Game setting" leaves a
 * cheat to the game's own Cheats menu. */
#ifndef LAUNCHER_CHEATS_H
#define LAUNCHER_CHEATS_H

#include <windows.h>

#define CHEATS_ENV_COUNT 5

void Cheats_Load(LPCWSTR iniPath);
void Cheats_Save(LPCWSTR iniPath);
BOOL Cheats_IsKnownIniKey(LPCWSTR key);
/* Environment entry i (0..CHEATS_ENV_COUNT-1): name, and value when set.
 * Returns FALSE when the variable is to be unset (the game decides). */
BOOL Cheats_Env(int i, LPCWSTR *name, LPWSTR value, size_t cap);
int  Cheats_ActiveCount(void);
/* Create the "Cheats..." button just left of the Play button. */
HWND Cheats_CreateButton(HWND parent, HINSTANCE inst, int id, UINT dpi);
/* Show "Cheats..." / "Cheats (2 on)..." on the button. */
void Cheats_UpdateButton(HWND button);
/* The cheats window, modal over owner. TRUE = the player pressed OK. */
BOOL Cheats_ShowDialog(HWND owner, HFONT font, UINT dpi);

#endif
