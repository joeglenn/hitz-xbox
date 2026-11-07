/* The game's built-in cheats in the launcher. See launcher_cheats.h and
 * src/cheats.h (what each cheat does, where the game keeps it).
 *
 * This module owns the state of the 5 game cheats (k_cheats[]) and the
 * small modal "Cheats" dialog that lets the player choose them, separately
 * from hitz_launcher.c's g_settings table because a cheat's value space
 * (up to 12 choices including "Game setting") does not fit the launcher's
 * fixed boolean/ini-enum SettingDef shape, and because the cheats button
 * and dialog are self-contained enough to live in their own file.
 *
 * Persistence: each cheat is one ini key (cheat_*) in the same
 * hitz_settings.ini as the rest of the settings, loaded/saved by
 * hitz_launcher.c calling Cheats_Load/Cheats_Save alongside its own
 * LoadSettings/SaveSettings.
 *
 * Environment variables: each active (non "Game setting") cheat becomes one
 * HITZ_CHEAT_* variable for the child game process, built through
 * Cheats_Env and folded into hitz_launcher.c's BuildEnvList.
 *
 * Threading: single-threaded, UI thread only. Cheats_ShowDialog runs its
 * own local GetMessageW loop while the dialog is open (disabling the owner
 * window, like a classic Win32 modal dialog), returning only once the
 * dialog is destroyed. */
#include <windows.h>
#include <strsafe.h>

#include "launcher_cheats.h"

/* One row of the cheat table: ini/env names, the label shown in the
 * launcher, the number of choices (including "Game setting" as choice 0),
 * the ini/env value and display text for each choice, and the code the
 * player would otherwise type into the game's own Cheats menu. count is
 * always <= ARRAYSIZE(values)/ARRAYSIZE(display) (12). */
typedef struct {
    LPCWSTR ini_key;
    LPCWSTR env_name;
    LPCWSTR caption;
    int count;                    /* choices, "Game setting" first */
    LPCWSTR values[12];           /* ini and env value; [0] = L"game" */
    LPCWSTR display[12];
    LPCWSTR code;                 /* the code the game itself takes */
} CheatDef;

static const CheatDef k_cheats[CHEATS_ENV_COUNT] = {
    { L"cheat_player_head", L"HITZ_CHEAT_PLAYER_HEAD", L"Player heads:", 4,
      { L"game", L"normal", L"big", L"huge" },
      { L"Game setting", L"Normal", L"Big", L"Huge" }, L"HERK" },
    { L"cheat_team_head", L"HITZ_CHEAT_TEAM_HEAD", L"Team heads:", 4,
      { L"game", L"normal", L"big", L"huge" },
      { L"Game setting", L"Normal", L"Big", L"Huge" }, L"INGY" },
    { L"cheat_puck_size", L"HITZ_CHEAT_PUCK_SIZE", L"Puck size:", 12,
      { L"game", L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"10" },
      { L"Game setting", L"Normal", L"1 (+15%)", L"2 (+30%)", L"3 (+45%)", L"4 (+60%)", L"5 (+75%)",
        L"6 (+90%)", L"7 (+105%)", L"8 (+120%)", L"9 (+135%)", L"10 (+150%, biggest)" }, L"211S" },
    { L"cheat_puck_shadow", L"HITZ_CHEAT_PUCK_SHADOW", L"Puck shadow:", 3,
      { L"game", L"off", L"on" },
      { L"Game setting", L"Off", L"On" }, L"SASG" },
    { L"cheat_puck_glow", L"HITZ_CHEAT_PUCK_GLOW", L"Puck glow:", 6,
      { L"game", L"off", L"green", L"yellow", L"red", L"blue" },
      { L"Game setting", L"Off", L"Green", L"Yellow", L"Red", L"Blue" }, L"CARB" },
};

static const wchar_t k_section[] = L"settings";       /* hitz_settings.ini, as the launcher */
/* Currently selected choice per cheat, index into k_cheats[i].values/display;
 * 0 always means "Game setting" (leave it to the game's own Cheats menu). */
static int s_sel[CHEATS_ENV_COUNT];          /* index into values; 0 = game */

/* Returns the index of v within d->values (case-insensitive), or 0 ("game")
 * if v does not match any known choice, e.g. an ini edited by hand. */
static int FindValue(const CheatDef *d, LPCWSTR v)
{
    for (int j = 0; j < d->count; j++) {
        if (_wcsicmp(v, d->values[j]) == 0) {
            return j;
        }
    }
    return 0;
}

/* Loads every cheat's selection from the ini file at iniPath into s_sel,
 * defaulting to "game" (index 0) for a missing key or an unrecognised
 * value. Called by hitz_launcher.c's LoadSettings. */
void Cheats_Load(LPCWSTR iniPath)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        wchar_t v[32];
        GetPrivateProfileStringW(k_section, k_cheats[i].ini_key, L"game", v, ARRAYSIZE(v), iniPath);
        s_sel[i] = FindValue(&k_cheats[i], v);
    }
}

/* Writes every cheat's current selection (s_sel) back to the ini file at
 * iniPath. Called by hitz_launcher.c's SaveSettings, and directly from
 * WndProc right after a successful Cheats_ShowDialog so the choice is not
 * lost if the player then quits without pressing Play/Quit again. */
void Cheats_Save(LPCWSTR iniPath)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        WritePrivateProfileStringW(k_section, k_cheats[i].ini_key, k_cheats[i].values[s_sel[i]], iniPath);
    }
}

/* Returns TRUE if key is one of the 5 cheat_* ini keys, so hitz_launcher.c's
 * --set command-line option can accept cheat keys too. */
BOOL Cheats_IsKnownIniKey(LPCWSTR key)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        if (_wcsicmp(key, k_cheats[i].ini_key) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Fills in the environment variable name and value for cheat i. Returns
 * FALSE (and an empty value) when the cheat is set to "Game setting" (index
 * 0), telling the caller to leave the variable unset so the game falls back
 * to its own Cheats menu; otherwise returns TRUE with the chosen value
 * copied into value (truncated-safe via StringCchCopyW, capacity cap). */
BOOL Cheats_Env(int i, LPCWSTR *name, LPWSTR value, size_t cap)
{
    *name = k_cheats[i].env_name;
    value[0] = L'\0';
    if (s_sel[i] == 0) {
        return FALSE;
    }
    StringCchCopyW(value, cap, k_cheats[i].values[s_sel[i]]);
    return TRUE;
}

/* Counts how many of the 5 cheats are set to something other than "Game
 * setting" (s_sel[i] != 0); used for the "Cheats (N on)..." button label. */
int Cheats_ActiveCount(void)
{
    int n = 0;
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        n += s_sel[i] != 0;
    }
    return n;
}

/* Scales a 96-DPI layout value to pixels for dpi; the local equivalent of
 * hitz_launcher.c's ScaleDpi/SX, kept as its own small helper since this
 * file does not include hitz_launcher.c. */
static int Px(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

/* Sets the Cheats button's text to "Cheats..." (nothing active) or
 * "Cheats (N on)..." (N cheats set away from "Game setting"). Called once
 * when the button is created and again after the player presses OK in the
 * dialog. */
void Cheats_UpdateButton(HWND button)
{
    wchar_t t[40];
    int n = Cheats_ActiveCount();
    if (n == 0) {
        StringCchCopyW(t, ARRAYSIZE(t), L"Cheats...");
    } else {
        StringCchPrintfW(t, ARRAYSIZE(t), L"Cheats (%d on)...", n);
    }
    SetWindowTextW(button, t);
}

/* Creates the "Cheats..." push button positioned just to the left of the
 * Play button (control id 1014, IDC_PLAY in hitz_launcher.c; hardcoded here
 * rather than shared via the header, since launcher_cheats.h intentionally
 * has no dependency on hitz_launcher.c's control-id defines), and sets its
 * initial label via Cheats_UpdateButton. Returns NULL if the Play button
 * cannot be found or its rect cannot be read (should not happen: the
 * launcher always creates Play before calling this). */
HWND Cheats_CreateButton(HWND parent, HINSTANCE inst, int id, UINT dpi)
{
    HWND play = GetDlgItem(parent, 1014);           /* IDC_PLAY */
    RECT r;
    HWND b;
    int w = Px(120, dpi), gap = Px(12, dpi);
    if (!play || !GetWindowRect(play, &r)) {
        return NULL;
    }
    MapWindowPoints(NULL, parent, (POINT *)&r, 2);
    b = CreateWindowExW(0, L"BUTTON", L"Cheats...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        r.left - gap - w, r.top, w, r.bottom - r.top, parent,
                        (HMENU)(INT_PTR)id, inst, NULL);
    Cheats_UpdateButton(b);
    return b;
}

/* ---------------------------------------------------------------- window */

#define IDC_CH_COMBO0 2100
#define IDC_CH_OK     2110
#define IDC_CH_CANCEL 2111
#define IDC_CH_RESET  2112

/* State for the one cheats dialog that can be open at a time: the combo box
 * handle for each cheat, whether the message loop in Cheats_ShowDialog
 * should stop (done, set by WM_DESTROY) and whether the player confirmed
 * with OK (ok, the dialog's return value). Zeroed at the top of every
 * Cheats_ShowDialog call. */
static struct {
    HWND combo[CHEATS_ENV_COUNT];
    BOOL done, ok;
} s_dlg;

/* Copies every combo box's current selection into s_sel (clamped to 0 if
 * the control reports an error or an out-of-range index), committing the
 * dialog's in-progress choices. Called when OK is pressed, before the
 * dialog is destroyed. */
static void DlgSave(void)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        LRESULT c = SendMessageW(s_dlg.combo[i], CB_GETCURSEL, 0, 0);
        s_sel[i] = (c >= 0 && c < k_cheats[i].count) ? (int)c : 0;
    }
}

/* Window procedure for the Cheats dialog. OK (button or IDOK, e.g. Enter)
 * saves the selections and closes; Cancel (button or IDCANCEL, e.g. Esc)
 * closes without saving; "All off" resets every combo to index 0 ("Game
 * setting") without closing the dialog. WM_CLOSE (the system close button)
 * also just destroys the window (acting like Cancel). WM_DESTROY marks the
 * dialog done so Cheats_ShowDialog's message loop can exit. */
static LRESULT CALLBACK CheatsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CH_OK:
        case IDOK:
            DlgSave();
            s_dlg.ok = TRUE;
            DestroyWindow(hwnd);
            return 0;
        case IDC_CH_CANCEL:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_CH_RESET:
            for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
                SendMessageW(s_dlg.combo[i], CB_SETCURSEL, 0, 0);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        s_dlg.done = TRUE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Thin CreateWindowExW wrapper for a dialog child control (label, combo or
 * button), applying the shared UI font (f) if given. Always WS_CHILD |
 * WS_VISIBLE plus the caller's extra style bits. */
static HWND Child(HWND p, LPCWSTR cls, LPCWSTR text, DWORD style, int id, int x, int y, int w, int h, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrW(p, GWLP_HINSTANCE), NULL);
    if (c && f) {
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    }
    return c;
}

/* Builds and runs the modal-style Cheats dialog: registers the window class
 * once (GetClassInfoExW guards against re-registering on a later call),
 * creates the popup window centred under owner, lays out one label + combo
 * + "(code XXXX)" row per cheat plus the explanatory text, "All off", OK and
 * Cancel buttons, disables owner (classic modal behaviour) and pumps this
 * window's own message loop until s_dlg.done is set by WM_DESTROY. A
 * WM_QUIT seen while the dialog is open is reposted so the main window's
 * loop still sees it after this function returns. Re-enables and refocuses
 * owner before returning. Returns FALSE immediately, without creating or
 * showing anything, if the window itself could not be created. Returns
 * TRUE only if the player pressed OK (s_dlg.ok); Cancel, the system close
 * button and Esc all return FALSE with s_sel left untouched by this call
 * (DlgSave is only reached on OK). */
BOOL Cheats_ShowDialog(HWND owner, HFONT font, UINT dpi)
{
    static const wchar_t cls[] = L"HitzCheatsWindow";
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    WNDCLASSEXW wc;
    RECT ro, rc;
    HWND w;
    MSG m;
    int cw = Px(460, dpi), y, x0 = Px(16, dpi), xc = Px(150, dpi);

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    if (!GetClassInfoExW(inst, cls, &wc)) {
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = CheatsWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
    }
    ZeroMemory(&s_dlg, sizeof s_dlg);

    rc.left = 0; rc.top = 0; rc.right = cw; rc.bottom = Px(372, dpi);
    AdjustWindowRectEx(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(owner, &ro);
    w = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Cheats", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                        ro.left + ((ro.right - ro.left) - (rc.right - rc.left)) / 2,
                        ro.top + Px(60, dpi), rc.right - rc.left, rc.bottom - rc.top,
                        owner, NULL, inst, NULL);
    if (!w) {
        return FALSE;
    }

    y = Px(14, dpi);
    Child(w, L"STATIC", L"The game's own cheats, without typing the codes as profile names. "
          L"\"Game setting\" leaves a cheat to the game's Cheats menu "
          L"(Settings > Game Tuning > Visuals > Cheats).",
          0, -1, x0, y, cw - 2 * x0, Px(48, dpi), font);
    y += Px(58, dpi);
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        wchar_t code[24];
        Child(w, L"STATIC", k_cheats[i].caption, 0, -1, x0, y + Px(3, dpi), xc - x0, Px(18, dpi), font);
        s_dlg.combo[i] = Child(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                               IDC_CH_COMBO0 + i, xc, y, Px(190, dpi), Px(300, dpi), font);
        for (int j = 0; j < k_cheats[i].count; j++) {
            SendMessageW(s_dlg.combo[i], CB_ADDSTRING, 0, (LPARAM)k_cheats[i].display[j]);
        }
        SendMessageW(s_dlg.combo[i], CB_SETCURSEL, (WPARAM)s_sel[i], 0);
        StringCchPrintfW(code, ARRAYSIZE(code), L"(code %s)", k_cheats[i].code);
        Child(w, L"STATIC", code, 0, -1, xc + Px(200, dpi), y + Px(3, dpi), Px(100, dpi), Px(18, dpi), font);
        y += Px(32, dpi);
    }
    y += Px(4, dpi);
    Child(w, L"STATIC", L"Puck size also changes how the puck plays (its collision size). "
          L"Online, both players get the host's cheats.",
          0, -1, x0, y, cw - 2 * x0, Px(34, dpi), font);
    y += Px(46, dpi);
    Child(w, L"BUTTON", L"All off", BS_PUSHBUTTON | WS_TABSTOP, IDC_CH_RESET, x0, y, Px(90, dpi), Px(28, dpi), font);
    Child(w, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_CH_OK, cw - Px(206, dpi), y, Px(90, dpi), Px(28, dpi), font);
    Child(w, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDC_CH_CANCEL, cw - Px(106, dpi), y, Px(90, dpi), Px(28, dpi), font);

    EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(s_dlg.combo[0]);
    while (!s_dlg.done) {
        BOOL r = GetMessageW(&m, NULL, 0, 0);
        if (r == 0) {                          /* WM_QUIT: hand it back to the main loop */
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (r < 0) {
            break;
        }
        if (!IsWindow(w) || !IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (IsWindow(w)) {
        DestroyWindow(w);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return s_dlg.ok;
}
