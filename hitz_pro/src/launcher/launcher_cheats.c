/* The game's built-in cheats in the launcher. See launcher_cheats.h and
 * src/cheats.h (what each cheat does, where the game keeps it). */
#include <windows.h>
#include <strsafe.h>

#include "launcher_cheats.h"

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
static int s_sel[CHEATS_ENV_COUNT];          /* index into values; 0 = game */

static int FindValue(const CheatDef *d, LPCWSTR v)
{
    for (int j = 0; j < d->count; j++) {
        if (_wcsicmp(v, d->values[j]) == 0) {
            return j;
        }
    }
    return 0;
}

void Cheats_Load(LPCWSTR iniPath)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        wchar_t v[32];
        GetPrivateProfileStringW(k_section, k_cheats[i].ini_key, L"game", v, ARRAYSIZE(v), iniPath);
        s_sel[i] = FindValue(&k_cheats[i], v);
    }
}

void Cheats_Save(LPCWSTR iniPath)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        WritePrivateProfileStringW(k_section, k_cheats[i].ini_key, k_cheats[i].values[s_sel[i]], iniPath);
    }
}

BOOL Cheats_IsKnownIniKey(LPCWSTR key)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        if (_wcsicmp(key, k_cheats[i].ini_key) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

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

int Cheats_ActiveCount(void)
{
    int n = 0;
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        n += s_sel[i] != 0;
    }
    return n;
}

static int Px(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

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

static struct {
    HWND combo[CHEATS_ENV_COUNT];
    BOOL done, ok;
} s_dlg;

static void DlgSave(void)
{
    for (int i = 0; i < CHEATS_ENV_COUNT; i++) {
        LRESULT c = SendMessageW(s_dlg.combo[i], CB_GETCURSEL, 0, 0);
        s_sel[i] = (c >= 0 && c < k_cheats[i].count) ? (int)c : 0;
    }
}

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

static HWND Child(HWND p, LPCWSTR cls, LPCWSTR text, DWORD style, int id, int x, int y, int w, int h, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrW(p, GWLP_HINSTANCE), NULL);
    if (c && f) {
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    }
    return c;
}

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
