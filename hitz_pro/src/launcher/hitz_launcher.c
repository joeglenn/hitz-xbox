/*
 * hitz_launcher.c - NHL Hitz Pro (Xbox recompilation) settings launcher.
 *
 * Plain Win32 + common controls, C11, no MFC/ATL/C++, no external libs.
 * Replaces editing run_hitz.bat / "set VAR=..." for non-technical users.
 *
 * Build (MSVC, from a Developer Command Prompt / after vcvars64.bat):
 *   cl /nologo /W4 /WX /O2 /utf-8 /DUNICODE /D_UNICODE hitz_launcher.c /link
 *      /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib shell32.lib
 *      dxgi.lib xinput.lib ole32.lib windowscodecs.lib
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WINVER 0x0A00

#include <windows.h>
#include <commctrl.h>
#define COBJMACROS
#include <wincodec.h>
#include <shellapi.h>
#include <strsafe.h>
#include <xinput.h>
#include <stdio.h>
#include <wchar.h>

#define INITGUID
#include <dxgi.h>
#undef INITGUID

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* ------------------------------------------------------------------ */
/* Dynamically-loaded DPI APIs (so the binary still runs on older      */
/* Windows; we fall back gracefully when they are absent).             */
/* ------------------------------------------------------------------ */

typedef BOOL(WINAPI *SetProcessDpiAwarenessContextProc)(HANDLE);
typedef UINT(WINAPI *GetDpiForWindowProc)(HWND);
typedef BOOL(WINAPI *SystemParametersInfoForDpiProc)(UINT, UINT, PVOID, UINT, UINT);

#define HITZ_DPI_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)(-4))

#ifndef ERROR_ENVIRONMENT_VARIABLE_NOT_FOUND
#define ERROR_ENVIRONMENT_VARIABLE_NOT_FOUND 203L
#endif

static GetDpiForWindowProc g_pGetDpiForWindow = NULL;
static SystemParametersInfoForDpiProc g_pSystemParametersInfoForDpi = NULL;

static void InitDpiSupport(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == NULL) {
        return;
    }

    SetProcessDpiAwarenessContextProc setCtx =
        (SetProcessDpiAwarenessContextProc)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (setCtx != NULL) {
        if (!setCtx(HITZ_DPI_CONTEXT_PER_MONITOR_AWARE_V2)) {
            SetProcessDPIAware();
        }
    } else {
        SetProcessDPIAware();
    }

    g_pGetDpiForWindow = (GetDpiForWindowProc)(void *)GetProcAddress(user32, "GetDpiForWindow");
    g_pSystemParametersInfoForDpi =
        (SystemParametersInfoForDpiProc)(void *)GetProcAddress(user32, "SystemParametersInfoForDpi");
}

static UINT DpiForWindow(HWND hwnd)
{
    if (g_pGetDpiForWindow != NULL) {
        UINT dpi = g_pGetDpiForWindow(hwnd);
        if (dpi != 0) {
            return dpi;
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = 96;
    if (hdc != NULL) {
        dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
    }
    return (UINT)((dpi > 0) ? dpi : 96);
}

static int ScaleDpi(int value, UINT dpi)
{
    return MulDiv(value, (int)dpi, 96);
}

/* ------------------------------------------------------------------ */
/* Error reporting                                                     */
/* ------------------------------------------------------------------ */

static void ShowWin32Error(HWND owner, LPCWSTR context)
{
    DWORD err = GetLastError();
    wchar_t *msgBuf = NULL;
    wchar_t finalBuf[768];

    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msgBuf, 0, NULL);

    if (msgBuf != NULL) {
        StringCchPrintfW(finalBuf, ARRAYSIZE(finalBuf), L"%s\n\n(Error %lu) %s", context, err, msgBuf);
        LocalFree(msgBuf);
    } else {
        StringCchPrintfW(finalBuf, ARRAYSIZE(finalBuf), L"%s\n\n(Error %lu)", context, err);
    }

    MessageBoxW(owner, finalBuf, L"NHL Hitz Pro", MB_OK | MB_ICONERROR);
}

/* ------------------------------------------------------------------ */
/* Settings table                                                      */
/* ------------------------------------------------------------------ */

#define IDC_MODE        1001
#define IDC_SCALE       1002
#define IDC_PICTURE     1003
#define IDC_PACING      1004
#define IDC_REFRESH     1005
#define IDC_GPU         1006
#define IDC_PLAYERS     1007
#define IDC_KEYBOARD    1008
#define IDC_SDLPADS     1009
#define IDC_DEBUG       1010
#define IDC_CTRLLIST    1011
#define IDC_CTRLREFRESH 1012
#define IDC_OPENLOG     1013
#define IDC_PLAY        1014
#define IDC_QUIT        1015

typedef struct {
    LPCWSTR ini_key;            /* key name inside hitz_settings.ini    */
    LPCWSTR env_name;           /* environment variable to set          */
    LPCWSTR default_ini_value;  /* default stored value                 */
    const LPCWSTR *ini_values;  /* allowed ini values, or NULL for bool */
    const LPCWSTR *env_values;  /* parallel env values, or NULL = same  */
    const LPCWSTR *display_values; /* combo box text, or NULL for bool  */
    int count;                  /* number of enum entries; 0 = checkbox */
    LPCWSTR caption;             /* label text (combo) / checkbox text   */
    int control_id;
} SettingDef;

static const LPCWSTR k_mode_ini[]    = { L"windowed", L"fullscreen" };
static const LPCWSTR k_mode_env[]    = { L"0", L"1" };
static const LPCWSTR k_mode_disp[]   = { L"Windowed", L"Fullscreen" };

static const LPCWSTR k_scale_ini[]   = { L"1", L"2", L"3", L"4" };
static const LPCWSTR k_scale_disp[]  = { L"1x", L"2x", L"3x", L"4x" };

static const LPCWSTR k_picture_ini[] = { L"smooth", L"sharp" };
static const LPCWSTR k_picture_env[] = { L"linear", L"point" };
static const LPCWSTR k_picture_disp[] = { L"Smooth", L"Sharp pixels" };

static const LPCWSTR k_pacing_ini[]  = { L"auto", L"timer" };
static const LPCWSTR k_pacing_disp[] = { L"Match display (recommended)", L"Game timer (old behaviour)" };

/* the game has two controller slots in play: more ports only confuse it */
static const LPCWSTR k_players_ini[] = { L"1", L"2" };
static const LPCWSTR k_players_disp[] = { L"1", L"2" };

/* Index order below matches the SettingIndex enum. Keep in sync. */
static const SettingDef g_settings[] = {
    { L"mode",           L"RECOMP_FULLSCREEN", L"windowed", k_mode_ini,    k_mode_env,    k_mode_disp,
      2, L"Mode:", IDC_MODE },
    { L"window_scale",   L"RECOMP_WINDOW_SCALE", L"2",      k_scale_ini,   NULL,          k_scale_disp,
      4, L"Window size:", IDC_SCALE },
    { L"picture",        L"RECOMP_PRESENT_FILTER", L"smooth", k_picture_ini, k_picture_env, k_picture_disp,
      2, L"Picture:", IDC_PICTURE },
    { L"pacing",         L"HITZ_PACING", L"auto",           k_pacing_ini,  NULL,          k_pacing_disp,
      2, L"Frame pacing:", IDC_PACING },
    { L"refresh_switch", L"HITZ_REFRESH_SWITCH", L"1",      NULL, NULL, NULL,
      0, L"Fullscreen: switch the monitor to 60/120 Hz (smoother)", IDC_REFRESH },
    { L"players",        L"RECOMP_USB_PADS", L"2",          k_players_ini, NULL,          k_players_disp,
      2, L"Players:", IDC_PLAYERS },
    { L"keyboard",       L"RECOMP_KEYBOARD", L"0",          NULL, NULL, NULL,
      0, L"Keyboard controls player 1", IDC_KEYBOARD },
    { L"sdl_pads",       L"HITZ_SDL_PADS", L"1",            NULL, NULL, NULL,
      0, L"Non-Xbox controllers (8BitDo, PlayStation, Switch)", IDC_SDLPADS },
    { L"debug",          L"HITZ_DEBUG", L"0",               NULL, NULL, NULL,
      0, L"Debug logging (hitz_stderr.log, hitz_perf.log)", IDC_DEBUG },
};

#define SETTING_COUNT (int)(sizeof(g_settings) / sizeof(g_settings[0]))

/* GPU adapter is handled separately: its value space is dynamic (the
 * list of installed GPUs), not a fixed enum known at compile time. */
static const LPCWSTR k_gpu_ini_key  = L"gpu_adapter";
static const LPCWSTR k_gpu_env_name = L"HITZ_GPU_ADAPTER";

/* Env vars always set by run_hitz.bat today, applied only when the
 * launcher's own environment does not already define them. */
typedef struct {
    LPCWSTR name;
    LPCWSTR value;
} StaticEnvEntry;

static const StaticEnvEntry g_static_env[] = {
    { L"RECOMP_PB_EXEC",    L"1" },
    { L"RECOMP_VBLANK",     L"paced" },
    { L"RECOMP_AC97_READY", L"1" },
    { L"RECOMP_FB_WINDOW",  L"1" },
    { L"RECOMP_D3D_PRESENT",L"1" },
    { L"RECOMP_USB",        L"1" },
    { L"HITZ_GPU",          L"1" },
};

#define STATIC_ENV_COUNT (int)(sizeof(g_static_env) / sizeof(g_static_env[0]))

#define SETTING_VALUE_CAP 128
#define GPU_VALUE_CAP 256

typedef struct {
    wchar_t values[SETTING_COUNT][SETTING_VALUE_CAP];
    wchar_t gpu_adapter[GPU_VALUE_CAP]; /* empty string means "Automatic" */
} AppSettings;

typedef struct {
    wchar_t name[64];
    wchar_t value[GPU_VALUE_CAP];
    BOOL unset; /* TRUE => unset the variable rather than set it */
} EnvEntry;

#define MAX_ENV_ENTRIES (SETTING_COUNT + 1 + STATIC_ENV_COUNT)

/* ------------------------------------------------------------------ */
/* Application paths                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t hitz_dir[MAX_PATH];
    wchar_t ini_path[MAX_PATH];
    wchar_t game_exe[MAX_PATH];
    wchar_t xbe_path[MAX_PATH];
    wchar_t log_path[MAX_PATH];
    BOOL game_found;
} AppPaths;

static BOOL FileExistsW(LPCWSTR path)
{
    DWORD attrs = GetFileAttributesW(path);
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static BOOL ResolvePaths(AppPaths *paths)
{
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return FALSE;
    }

    wchar_t dir1[MAX_PATH];
    StringCchCopyW(dir1, MAX_PATH, exePath);
    wchar_t *slash1 = wcsrchr(dir1, L'\\');
    if (slash1 != NULL) {
        *slash1 = L'\0';
    }

    wchar_t candidate[MAX_PATH];
    StringCchPrintfW(candidate, MAX_PATH, L"%s\\build\\hitz_pro_recomp.exe", dir1);

    if (FileExistsW(candidate)) {
        StringCchCopyW(paths->hitz_dir, MAX_PATH, dir1);
    } else {
        wchar_t dir2[MAX_PATH];
        StringCchCopyW(dir2, MAX_PATH, dir1);
        wchar_t *slash2 = wcsrchr(dir2, L'\\');
        if (slash2 != NULL) {
            *slash2 = L'\0';
        }
        StringCchPrintfW(candidate, MAX_PATH, L"%s\\build\\hitz_pro_recomp.exe", dir2);
        if (FileExistsW(candidate)) {
            StringCchCopyW(paths->hitz_dir, MAX_PATH, dir2);
        } else {
            StringCchCopyW(paths->hitz_dir, MAX_PATH, dir1);
        }
    }

    if (FAILED(StringCchPrintfW(paths->ini_path, MAX_PATH, L"%s\\hitz_settings.ini", paths->hitz_dir))) return FALSE;   /* path too long */
    if (FAILED(StringCchPrintfW(paths->game_exe, MAX_PATH, L"%s\\build\\hitz_pro_recomp.exe", paths->hitz_dir))) return FALSE;   /* path too long */
    if (FAILED(StringCchPrintfW(paths->xbe_path, MAX_PATH, L"%s\\game\\default.xbe", paths->hitz_dir))) return FALSE;   /* path too long */
    if (FAILED(StringCchPrintfW(paths->log_path, MAX_PATH, L"%s\\hitz_stderr.log", paths->hitz_dir))) return FALSE;   /* path too long */

    paths->game_found = FileExistsW(paths->game_exe) && FileExistsW(paths->xbe_path);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Settings load / save                                                */
/* ------------------------------------------------------------------ */

static const wchar_t *k_ini_section = L"settings";

static void LoadSettings(LPCWSTR iniPath, AppSettings *out)
{
    for (int i = 0; i < SETTING_COUNT; i++) {
        const SettingDef *d = &g_settings[i];
        GetPrivateProfileStringW(k_ini_section, d->ini_key, d->default_ini_value,
                                  out->values[i], SETTING_VALUE_CAP, iniPath);
    }
    GetPrivateProfileStringW(k_ini_section, k_gpu_ini_key, L"",
                              out->gpu_adapter, GPU_VALUE_CAP, iniPath);
}

static void SaveSettings(LPCWSTR iniPath, const AppSettings *in)
{
    for (int i = 0; i < SETTING_COUNT; i++) {
        const SettingDef *d = &g_settings[i];
        WritePrivateProfileStringW(k_ini_section, d->ini_key, in->values[i], iniPath);
    }
    WritePrivateProfileStringW(k_ini_section, k_gpu_ini_key, in->gpu_adapter, iniPath);
}

/* Map a stored ini value to the environment-variable value for a given
 * setting definition (handles both enum mappings and plain booleans). */
static void SettingEnvValue(const SettingDef *d, LPCWSTR iniValue, LPWSTR out, size_t outCap)
{
    if (d->count == 0) {
        StringCchCopyW(out, outCap, iniValue);
        return;
    }
    for (int i = 0; i < d->count; i++) {
        if (_wcsicmp(iniValue, d->ini_values[i]) == 0) {
            LPCWSTR envVal = (d->env_values != NULL) ? d->env_values[i] : d->ini_values[i];
            StringCchCopyW(out, outCap, envVal);
            return;
        }
    }
    for (int i = 0; i < d->count; i++) {
        if (_wcsicmp(d->default_ini_value, d->ini_values[i]) == 0) {
            LPCWSTR envVal = (d->env_values != NULL) ? d->env_values[i] : d->ini_values[i];
            StringCchCopyW(out, outCap, envVal);
            return;
        }
    }
    out[0] = L'\0';
}

/* Build the full list of environment variables this launcher would set,
 * including the "only if not already defined" static defaults. Used by
 * both the real launch path and --print-env. */
static int BuildEnvList(const AppSettings *s, EnvEntry entries[MAX_ENV_ENTRIES])
{
    int n = 0;

    for (int i = 0; i < SETTING_COUNT; i++) {
        const SettingDef *d = &g_settings[i];
        StringCchCopyW(entries[n].name, ARRAYSIZE(entries[n].name), d->env_name);
        SettingEnvValue(d, s->values[i], entries[n].value, ARRAYSIZE(entries[n].value));
        entries[n].unset = FALSE;
        n++;
    }

    StringCchCopyW(entries[n].name, ARRAYSIZE(entries[n].name), k_gpu_env_name);
    if (s->gpu_adapter[0] == L'\0') {
        entries[n].value[0] = L'\0';
        entries[n].unset = TRUE;
    } else {
        StringCchCopyW(entries[n].value, ARRAYSIZE(entries[n].value), s->gpu_adapter);
        entries[n].unset = FALSE;
    }
    n++;

    for (int i = 0; i < STATIC_ENV_COUNT; i++) {
        wchar_t probe[4];
        DWORD r = GetEnvironmentVariableW(g_static_env[i].name, probe, ARRAYSIZE(probe));
        if (r == 0 && GetLastError() == ERROR_ENVIRONMENT_VARIABLE_NOT_FOUND) {
            StringCchCopyW(entries[n].name, ARRAYSIZE(entries[n].name), g_static_env[i].name);
            StringCchCopyW(entries[n].value, ARRAYSIZE(entries[n].value), g_static_env[i].value);
            entries[n].unset = FALSE;
            n++;
        }
    }

    return n;
}

static void ApplyEnvironment(const AppSettings *s)
{
    EnvEntry entries[MAX_ENV_ENTRIES];
    int n = BuildEnvList(s, entries);
    for (int i = 0; i < n; i++) {
        SetEnvironmentVariableW(entries[i].name, entries[i].unset ? NULL : entries[i].value);
    }
}

static void PrintEnvToStdout(const AppSettings *s)
{
    EnvEntry entries[MAX_ENV_ENTRIES];
    int n = BuildEnvList(s, entries);

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == NULL || out == INVALID_HANDLE_VALUE) {
        return;
    }

    for (int i = 0; i < n; i++) {
        wchar_t line[320];
        StringCchPrintfW(line, ARRAYSIZE(line), L"%s=%s\r\n", entries[i].name,
                          entries[i].unset ? L"" : entries[i].value);

        char ansi[640];
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, ansi, (int)sizeof(ansi), NULL, NULL);
        if (bytes > 1) {
            DWORD written = 0;
            WriteFile(out, ansi, (DWORD)(bytes - 1), &written, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Command-line handling (--set KEY=VALUE, --print-env)                */
/* ------------------------------------------------------------------ */

static BOOL IsKnownIniKey(LPCWSTR key)
{
    for (int i = 0; i < SETTING_COUNT; i++) {
        if (_wcsicmp(key, g_settings[i].ini_key) == 0) {
            return TRUE;
        }
    }
    return (_wcsicmp(key, k_gpu_ini_key) == 0);
}

/* Returns TRUE if --print-env was requested. Applies any --set options
 * to the ini file as a side effect. */
static BOOL ParseCommandLine(LPCWSTR iniPath)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL printEnv = FALSE;

    if (argv == NULL) {
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--print-env") == 0) {
            printEnv = TRUE;
        } else if (_wcsicmp(argv[i], L"--set") == 0 && (i + 1) < argc) {
            i++;
            LPWSTR pair = argv[i];
            wchar_t *eq = wcschr(pair, L'=');
            if (eq != NULL) {
                wchar_t key[64];
                size_t keyLen = (size_t)(eq - pair);
                if (keyLen >= ARRAYSIZE(key)) {
                    keyLen = ARRAYSIZE(key) - 1;
                }
                wmemcpy(key, pair, keyLen);
                key[keyLen] = L'\0';
                LPCWSTR value = eq + 1;
                if (IsKnownIniKey(key) && wcschr(value, L'\r') == NULL && wcschr(value, L'\n') == NULL) {
                    WritePrivateProfileStringW(k_ini_section, key, value, iniPath);
                }
            }
        }
    }

    LocalFree(argv);
    return printEnv;
}

/* ------------------------------------------------------------------ */
/* GPU adapter enumeration (DXGI)                                      */
/* ------------------------------------------------------------------ */

static void EnumerateGpuAdapters(HWND comboBox)
{
    SendMessageW(comboBox, CB_RESETCONTENT, 0, 0);
    SendMessageW(comboBox, CB_ADDSTRING, 0, (LPARAM)L"Automatic");

    IDXGIFactory1 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr) || factory == NULL) {
        return;
    }

    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *adapter = NULL;
        HRESULT hr2 = factory->lpVtbl->EnumAdapters1(factory, i, &adapter);
        if (hr2 == DXGI_ERROR_NOT_FOUND || FAILED(hr2) || adapter == NULL) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc;
        ZeroMemory(&desc, sizeof(desc));
        if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))) {
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                SendMessageW(comboBox, CB_ADDSTRING, 0, (LPARAM)desc.Description);
            }
        }
        adapter->lpVtbl->Release(adapter);
    }

    factory->lpVtbl->Release(factory);
}

/* ------------------------------------------------------------------ */
/* Controller enumeration (XInput + optional SDL2)                     */
/* ------------------------------------------------------------------ */

typedef int (*SDL_Init_t)(unsigned int flags);
typedef int (*SDL_NumJoysticks_t)(void);
typedef int (*SDL_IsGameController_t)(int index);
typedef const char *(*SDL_GameControllerNameForIndex_t)(int index);
typedef const char *(*SDL_JoystickNameForIndex_t)(int index);
typedef void (*SDL_Quit_t)(void);

#define SDL_INIT_GAMECONTROLLER 0x00002000u

static void RefreshControllerList(HWND listBox, LPCWSTR hitzDir)
{
    SendMessageW(listBox, LB_RESETCONTENT, 0, 0);
    int found = 0;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(state));
        if (XInputGetState(i, &state) == ERROR_SUCCESS) {
            wchar_t line[64];
            StringCchPrintfW(line, ARRAYSIZE(line), L"Xbox controller (XInput slot %lu)", i + 1);
            SendMessageW(listBox, LB_ADDSTRING, 0, (LPARAM)line);
            found++;
        }
    }

    wchar_t sdlPath[MAX_PATH];
    StringCchPrintfW(sdlPath, MAX_PATH, L"%s\\build\\SDL2.dll", hitzDir);
    if (FileExistsW(sdlPath)) {
        HMODULE sdl = LoadLibraryW(sdlPath);
        if (sdl != NULL) {
            SDL_Init_t sdlInit = (SDL_Init_t)(void *)GetProcAddress(sdl, "SDL_Init");
            SDL_NumJoysticks_t sdlNum = (SDL_NumJoysticks_t)(void *)GetProcAddress(sdl, "SDL_NumJoysticks");
            SDL_IsGameController_t sdlIsPad =
                (SDL_IsGameController_t)(void *)GetProcAddress(sdl, "SDL_IsGameController");
            SDL_GameControllerNameForIndex_t sdlPadName =
                (SDL_GameControllerNameForIndex_t)(void *)GetProcAddress(sdl, "SDL_GameControllerNameForIndex");
            SDL_JoystickNameForIndex_t sdlJoyName =
                (SDL_JoystickNameForIndex_t)(void *)GetProcAddress(sdl, "SDL_JoystickNameForIndex");
            SDL_Quit_t sdlQuit = (SDL_Quit_t)(void *)GetProcAddress(sdl, "SDL_Quit");

            if (sdlInit != NULL && sdlNum != NULL && sdlIsPad != NULL && sdlQuit != NULL) {
                if (sdlInit(SDL_INIT_GAMECONTROLLER) == 0) {
                    int count = sdlNum();
                    for (int i = 0; i < count; i++) {
                        const char *name = NULL;
                        if (sdlIsPad(i) && sdlPadName != NULL) {
                            name = sdlPadName(i);
                        } else if (sdlJoyName != NULL) {
                            name = sdlJoyName(i);
                        }
                        if (name != NULL) {
                            wchar_t wname[256];
                            MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, ARRAYSIZE(wname));
                            SendMessageW(listBox, LB_ADDSTRING, 0, (LPARAM)wname);
                            found++;
                        }
                    }
                    sdlQuit();
                }
            }
            FreeLibrary(sdl);
        }
    }

    if (found == 0) {
        SendMessageW(listBox, LB_ADDSTRING, 0, (LPARAM)L"No controllers found");
    }
}

/* ------------------------------------------------------------------ */
/* Launching the game                                                   */
/* ------------------------------------------------------------------ */

/* The "Debug logging" box. */
static BOOL DebugLogging(const AppSettings *settings)
{
    int i;
    for (i = 0; i < SETTING_COUNT; i++)
        if (lstrcmpW(g_settings[i].ini_key, L"debug") == 0)
            return lstrcmpW(settings->values[i], L"1") == 0;
    return FALSE;
}

static void LaunchGame(HWND mainWnd, const AppPaths *paths, const AppSettings *settings)
{
    const BOOL debug = DebugLogging(settings);
    ApplyEnvironment(settings);

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    /* Debug logging off: the game writes no log, so it gets NUL and the last
     * hitz_stderr.log is left as it was. */
    HANDLE logHandle = debug
        ? CreateFileW(paths->log_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
        : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logHandle == INVALID_HANDLE_VALUE) {
        ShowWin32Error(mainWnd, L"Could not create the log file.");
        return;
    }

    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logHandle;
    si.hStdError = logHandle;
    si.hStdInput = (nullInput != INVALID_HANDLE_VALUE) ? nullInput : NULL;

    BOOL ok = CreateProcessW(paths->game_exe, NULL, NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, paths->hitz_dir, &si, &pi);

    if (!ok) {
        ShowWin32Error(mainWnd, L"Could not start hitz_pro_recomp.exe.");
        CloseHandle(logHandle);
        if (nullInput != INVALID_HANDLE_VALUE) {
            CloseHandle(nullInput);
        }
        return;
    }

    CloseHandle(logHandle);
    if (nullInput != INVALID_HANDLE_VALUE) {
        CloseHandle(nullInput);
    }
    CloseHandle(pi.hThread);

    ShowWindow(mainWnd, SW_HIDE);

    for (;;) {
        DWORD wait = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                /* Let the outer loop see WM_QUIT too; re-post it. */
                PostQuitMessage((int)msg.wParam);
                goto wait_done;
            }
            if (!IsDialogMessageW(mainWnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
wait_done:
    ;

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    ShowWindow(mainWnd, SW_SHOW);

    if (exitCode != 0) {
        wchar_t msg[640];
        if (debug)
            StringCchPrintfW(msg, ARRAYSIZE(msg),
                              L"NHL Hitz Pro exited with code %lu.\n\nSee the log for details:\n%s",
                              exitCode, paths->log_path);
        else
            StringCchPrintfW(msg, ARRAYSIZE(msg),
                              L"NHL Hitz Pro exited with code %lu.\n\nA crash leaves hitz_crash.log in the "
                              L"log folder. For a full log, tick \"Debug logging\" and play again.",
                              exitCode);
        MessageBoxW(mainWnd, msg, L"NHL Hitz Pro", MB_OK | MB_ICONWARNING);
    }
}

/* ------------------------------------------------------------------ */
/* GUI                                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    AppPaths paths;
    AppSettings settings;
    HWND combo_or_check[SETTING_COUNT];
    HWND gpu_combo;
    HWND ctrl_list;
    HFONT font;
    UINT dpi;                    /* the DPI the controls are laid out for */
} AppState;

static AppState g_app;

static HWND MakeStatic(HWND parent, HINSTANCE inst, LPCWSTR text, int x, int y, int w, int h)
{
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                            x, y, w, h, parent, NULL, inst, NULL);
}

static HWND MakeGroupBox(HWND parent, HINSTANCE inst, LPCWSTR text, int x, int y, int w, int h)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                            x, y, w, h, parent, NULL, inst, NULL);
}

static HWND MakeButton(HWND parent, HINSTANCE inst, LPCWSTR text, int id, int x, int y, int w, int h, BOOL isDefault)
{
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    return CreateWindowExW(0, L"BUTTON", text, style, x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, NULL);
}

static HWND MakeCheckbox(HWND parent, HINSTANCE inst, LPCWSTR text, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, NULL);
}

static HWND MakeCombo(HWND parent, HINSTANCE inst, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(0, L"COMBOBOX", NULL,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, NULL);
}

static HWND MakeListBox(HWND parent, HINSTANCE inst, int id, int x, int y, int w, int h)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOSEL,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, NULL);
}

static void ApplyFontToChildren(HWND parent, HFONT font)
{
    HWND child = GetWindow(parent, GW_CHILD);
    while (child != NULL) {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static void LoadSettingIntoControl(HWND hwnd, int index)
{
    const SettingDef *d = &g_settings[index];
    HWND ctrl = g_app.combo_or_check[index];
    (void)hwnd;

    if (d->count == 0) {
        BOOL checked = (_wcsicmp(g_app.settings.values[index], L"1") == 0);
        SendMessageW(ctrl, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }

    for (int i = 0; i < d->count; i++) {
        if (_wcsicmp(g_app.settings.values[index], d->ini_values[i]) == 0) {
            SendMessageW(ctrl, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    for (int i = 0; i < d->count; i++) {
        if (_wcsicmp(d->default_ini_value, d->ini_values[i]) == 0) {
            SendMessageW(ctrl, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    SendMessageW(ctrl, CB_SETCURSEL, 0, 0);
}

static void SaveControlIntoSetting(int index)
{
    const SettingDef *d = &g_settings[index];
    HWND ctrl = g_app.combo_or_check[index];

    if (d->count == 0) {
        LRESULT checked = SendMessageW(ctrl, BM_GETCHECK, 0, 0);
        StringCchCopyW(g_app.settings.values[index], SETTING_VALUE_CAP,
                       (checked == BST_CHECKED) ? L"1" : L"0");
        return;
    }

    LRESULT sel = SendMessageW(ctrl, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        sel = 0;
    }
    if (sel < 0 || sel >= d->count) {
        sel = 0;
    }
    StringCchCopyW(g_app.settings.values[index], SETTING_VALUE_CAP, d->ini_values[sel]);
}

static void SaveAllControlsIntoSettings(void)
{
    for (int i = 0; i < SETTING_COUNT; i++) {
        SaveControlIntoSetting(i);
    }

    int sel = (int)SendMessageW(g_app.gpu_combo, CB_GETCURSEL, 0, 0);
    if (sel <= 0) {
        g_app.settings.gpu_adapter[0] = L'\0';
    } else {
        LRESULT len = SendMessageW(g_app.gpu_combo, CB_GETLBTEXTLEN, (WPARAM)sel, 0);
        if (len != CB_ERR && len < (LRESULT)ARRAYSIZE(g_app.settings.gpu_adapter)) {
            SendMessageW(g_app.gpu_combo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)g_app.settings.gpu_adapter);
        } else {
            g_app.settings.gpu_adapter[0] = L'\0';     /* too long to keep: Automatic */
        }
    }
}

static void UpdateScaleEnabled(HWND hwnd)
{
    (void)hwnd;
    LRESULT modeSel = SendMessageW(g_app.combo_or_check[0], CB_GETCURSEL, 0, 0);
    BOOL windowed = (modeSel == 0);
    EnableWindow(g_app.combo_or_check[1], windowed);
}

static void CreateAllControls(HWND hwnd, HINSTANCE inst, UINT dpi)
{
#define SX(v) ScaleDpi((v), dpi)

    MakeGroupBox(hwnd, inst, L"Display", SX(12), SX(10), SX(456), SX(216));
    MakeGroupBox(hwnd, inst, L"Controls", SX(12), SX(236), SX(456), SX(176));
    MakeGroupBox(hwnd, inst, L"Diagnostics", SX(12), SX(422), SX(456), SX(66));

    int y;

    /* Display group */
    y = SX(34);
    MakeStatic(hwnd, inst, g_settings[0].caption, SX(26), y + SX(3), SX(90), SX(18));
    g_app.combo_or_check[0] = MakeCombo(hwnd, inst, g_settings[0].control_id, SX(150), y, SX(150), SX(22));

    y = SX(64);
    MakeStatic(hwnd, inst, g_settings[1].caption, SX(26), y + SX(3), SX(110), SX(18));
    g_app.combo_or_check[1] = MakeCombo(hwnd, inst, g_settings[1].control_id, SX(150), y, SX(100), SX(22));

    y = SX(94);
    MakeStatic(hwnd, inst, g_settings[2].caption, SX(26), y + SX(3), SX(90), SX(18));
    g_app.combo_or_check[2] = MakeCombo(hwnd, inst, g_settings[2].control_id, SX(150), y, SX(150), SX(22));

    y = SX(124);
    MakeStatic(hwnd, inst, g_settings[3].caption, SX(26), y + SX(3), SX(110), SX(18));
    g_app.combo_or_check[3] = MakeCombo(hwnd, inst, g_settings[3].control_id, SX(150), y, SX(300), SX(22));

    y = SX(154);
    g_app.combo_or_check[4] = MakeCheckbox(hwnd, inst, g_settings[4].caption, g_settings[4].control_id,
                                            SX(26), y, SX(420), SX(20));

    y = SX(182);
    MakeStatic(hwnd, inst, L"GPU:", SX(26), y + SX(3), SX(90), SX(18));
    g_app.gpu_combo = MakeCombo(hwnd, inst, IDC_GPU, SX(150), y, SX(300), SX(22));

    /* Controls group */
    y = SX(260);
    MakeStatic(hwnd, inst, g_settings[5].caption, SX(26), y + SX(3), SX(70), SX(18));
    g_app.combo_or_check[5] = MakeCombo(hwnd, inst, g_settings[5].control_id, SX(150), y, SX(80), SX(22));

    y = SX(290);
    g_app.combo_or_check[6] = MakeCheckbox(hwnd, inst, g_settings[6].caption, g_settings[6].control_id,
                                            SX(26), y, SX(420), SX(20));

    y = SX(314);
    g_app.combo_or_check[7] = MakeCheckbox(hwnd, inst, g_settings[7].caption, g_settings[7].control_id,
                                            SX(26), y, SX(420), SX(20));

    y = SX(340);
    MakeStatic(hwnd, inst, L"Connected controllers:", SX(26), y, SX(200), SX(18));
    g_app.ctrl_list = MakeListBox(hwnd, inst, IDC_CTRLLIST, SX(26), y + SX(18), SX(330), SX(56));
    MakeButton(hwnd, inst, L"Refresh", IDC_CTRLREFRESH, SX(366), y + SX(18), SX(90), SX(26), FALSE);

    /* Diagnostics group */
    y = SX(446);
    g_app.combo_or_check[8] = MakeCheckbox(hwnd, inst, g_settings[8].caption, g_settings[8].control_id,
                                            SX(26), y, SX(420), SX(20));
    MakeButton(hwnd, inst, L"Open log folder", IDC_OPENLOG, SX(26), y + SX(26), SX(150), SX(26), FALSE);

    /* Bottom buttons */
    MakeButton(hwnd, inst, L"Play", IDC_PLAY, SX(276), SX(500), SX(90), SX(30), TRUE);
    MakeButton(hwnd, inst, L"Quit", IDC_QUIT, SX(378), SX(500), SX(90), SX(30), FALSE);

#undef SX
}

static void PopulateCombos(void)
{
    for (int i = 0; i < SETTING_COUNT; i++) {
        const SettingDef *d = &g_settings[i];
        if (d->count == 0) {
            continue;
        }
        HWND combo = g_app.combo_or_check[i];
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (int j = 0; j < d->count; j++) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)d->display_values[j]);
        }
        LoadSettingIntoControl(NULL, i);
    }
}

static void LoadBoolControls(void)
{
    for (int i = 0; i < SETTING_COUNT; i++) {
        if (g_settings[i].count == 0) {
            LoadSettingIntoControl(NULL, i);
        }
    }
}

static HFONT CreateUiFont(HWND hwnd, UINT dpi);

typedef struct {
    HWND parent;
    UINT from, to;
} RescaleCtx;

static BOOL CALLBACK RescaleOne(HWND child, LPARAM lParam)
{
    const RescaleCtx *c = (const RescaleCtx *)lParam;
    RECT rc;
    if (GetParent(child) != c->parent || !GetWindowRect(child, &rc)) {
        return TRUE;
    }
    MapWindowPoints(NULL, c->parent, (POINT *)&rc, 2);
    MoveWindow(child,
               MulDiv(rc.left, (int)c->to, (int)c->from),
               MulDiv(rc.top, (int)c->to, (int)c->from),
               MulDiv(rc.right - rc.left, (int)c->to, (int)c->from),
               MulDiv(rc.bottom - rc.top, (int)c->to, (int)c->from), TRUE);
    return TRUE;
}

/* Move and resize every control for a new DPI (the window was dragged to a
 * monitor with another scale factor). */
static void RescaleChildren(HWND hwnd, UINT fromDpi, UINT toDpi)
{
    RescaleCtx c;
    c.parent = hwnd;
    c.from = fromDpi;
    c.to = toDpi;
    EnumChildWindows(hwnd, RescaleOne, (LPARAM)&c);
}

/* ---- Cover panel ----
 * hitz_pro\\cover.jpg (or .png), if the player put one there, is drawn on the
 * left at the window's height; the controls move right to make room. The
 * image is decoded once with WIC and scaled (Fant) whenever the size changes. */
static IWICImagingFactory *s_wic;
static IWICBitmapSource   *s_cover;         /* 32bpp premultiplied BGRA */
static UINT                s_cover_w, s_cover_h;
static HBITMAP             s_cover_bmp;
static int                 s_cover_bw, s_cover_bh;

static int CoverLoad(const wchar_t *dir)
{
    static const wchar_t *names[] = { L"cover.jpg", L"cover.jpeg", L"cover.png" };
    wchar_t path[MAX_PATH];
    int i;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) && !s_wic) {
        /* already initialised another way: carry on */
    }
    if (!s_wic && FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                          &IID_IWICImagingFactory, (void **)&s_wic))) {
        s_wic = NULL;
        return 0;
    }
    for (i = 0; i < (int)ARRAYSIZE(names) && !s_cover; i++) {
        IWICBitmapDecoder *dec = NULL;
        IWICBitmapFrameDecode *frame = NULL;
        if (FAILED(StringCchPrintfW(path, MAX_PATH, L"%s\\%s", dir, names[i]))) continue;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(s_wic, path, NULL, GENERIC_READ,
                                                                    WICDecodeMetadataCacheOnDemand, &dec))
            && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))
            && SUCCEEDED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppPBGRA, (IWICBitmapSource *)frame, &s_cover))
            && (FAILED(IWICBitmapSource_GetSize(s_cover, &s_cover_w, &s_cover_h)) || !s_cover_w || !s_cover_h)) {
            IWICBitmapSource_Release(s_cover);
            s_cover = NULL;
        }
        if (frame) IWICBitmapFrameDecode_Release(frame);
        if (dec) IWICBitmapDecoder_Release(dec);
    }
    return s_cover != NULL;
}

static int CoverWidth(int clientH)
{
    return s_cover ? MulDiv(clientH, (int)s_cover_w, (int)s_cover_h) : 0;
}

static HBITMAP CoverBitmap(int w, int h)
{
    IWICBitmapScaler *sc = NULL;
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bmp;
    if (s_cover_bmp && s_cover_bw == w && s_cover_bh == h) return s_cover_bmp;
    if (s_cover_bmp) { DeleteObject(s_cover_bmp); s_cover_bmp = NULL; }
    if (!s_cover || w <= 0 || h <= 0 || FAILED(IWICImagingFactory_CreateBitmapScaler(s_wic, &sc))) return NULL;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                  /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits
        && SUCCEEDED(IWICBitmapScaler_Initialize(sc, s_cover, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant))
        && SUCCEEDED(IWICBitmapScaler_CopyPixels(sc, NULL, (UINT)w * 4, (UINT)w * 4 * (UINT)h, (BYTE *)bits))) {
        s_cover_bmp = bmp;
        s_cover_bw = w;
        s_cover_bh = h;
    } else if (bmp) {
        DeleteObject(bmp);
    }
    IWICBitmapScaler_Release(sc);
    return s_cover_bmp;
}

static BOOL CALLBACK ShiftOne(HWND child, LPARAM lParam)
{
    HWND parent = GetParent(child);
    RECT rc;
    if (!parent || !GetWindowRect(child, &rc)) return TRUE;
    if (parent != GetAncestor(child, GA_PARENT)) return TRUE;
    MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
    SetWindowPos(child, NULL, rc.left + (int)lParam, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return TRUE;
}

/* After the controls exist: with a cover, make the window wider and move
 * every control right by the panel's width. */
static void CoverLayout(HWND hwnd)
{
    RECT rc, wr;
    int pw;
    HWND child;
    if (!CoverLoad(g_app.paths.hitz_dir)) return;
    GetClientRect(hwnd, &rc);
    pw = CoverWidth(rc.bottom);
    for (child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        ShiftOne(child, pw);
    wr.left = 0; wr.top = 0; wr.right = rc.right + pw; wr.bottom = rc.bottom;
    AdjustWindowRectEx(&wr, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        UINT dpi = DpiForWindow(hwnd);

        g_app.dpi = dpi;
        CreateAllControls(hwnd, inst, dpi);
        PopulateCombos();
        LoadBoolControls();
        EnumerateGpuAdapters(g_app.gpu_combo);

        /* select stored GPU adapter if present in the list */
        int gpuSel = 0;
        if (g_app.settings.gpu_adapter[0] != L'\0') {
            LRESULT found = SendMessageW(g_app.gpu_combo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                          (LPARAM)g_app.settings.gpu_adapter);
            if (found != CB_ERR) {
                gpuSel = (int)found;
            }
        }
        SendMessageW(g_app.gpu_combo, CB_SETCURSEL, (WPARAM)gpuSel, 0);

        RefreshControllerList(g_app.ctrl_list, g_app.paths.hitz_dir);
        UpdateScaleEnabled(hwnd);
        ApplyFontToChildren(hwnd, g_app.font);
        CoverLayout(hwnd);

        if (!g_app.paths.game_found) {
            EnableWindow(GetDlgItem(hwnd, IDC_PLAY), FALSE);
            MessageBoxW(hwnd, L"Game files not found: run setup.bat first.",
                        L"NHL Hitz Pro", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_MODE && code == CBN_SELCHANGE) {
            UpdateScaleEnabled(hwnd);
            return 0;
        }
        if (id == IDC_CTRLREFRESH && code == BN_CLICKED) {
            RefreshControllerList(g_app.ctrl_list, g_app.paths.hitz_dir);
            return 0;
        }
        if (id == IDC_OPENLOG && code == BN_CLICKED) {
            ShellExecuteW(hwnd, L"explore", g_app.paths.hitz_dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (id == IDC_QUIT && code == BN_CLICKED) {
            SaveAllControlsIntoSettings();
            SaveSettings(g_app.paths.ini_path, &g_app.settings);
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_PLAY && code == BN_CLICKED) {
            SaveAllControlsIntoSettings();
            SaveSettings(g_app.paths.ini_path, &g_app.settings);
            LaunchGame(hwnd, &g_app.paths, &g_app.settings);
            return 0;
        }
        return 0;
    }

    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wParam);
        const RECT *suggested = (const RECT *)lParam;
        if (newDpi != 0 && g_app.dpi != 0 && newDpi != g_app.dpi) {
            RescaleChildren(hwnd, g_app.dpi, newDpi);
            HFONT old = g_app.font;
            g_app.font = CreateUiFont(hwnd, newDpi);
            ApplyFontToChildren(hwnd, g_app.font);
            if (old != NULL && old != g_app.font && old != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
                DeleteObject(old);
            }
            g_app.dpi = newDpi;
        }
        if (suggested != NULL) {
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_PAINT:
        if (s_cover) {
            PAINTSTRUCT ps;
            RECT rc;
            HDC dc = BeginPaint(hwnd, &ps);
            int pw;
            HBITMAP bmp;
            GetClientRect(hwnd, &rc);
            pw = CoverWidth(rc.bottom);
            bmp = CoverBitmap(pw, rc.bottom);
            if (bmp) {
                HDC mem = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(mem, bmp);
                BitBlt(dc, 0, 0, pw, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (s_cover_bmp) { DeleteObject(s_cover_bmp); s_cover_bmp = NULL; }
        if (s_cover) { IWICBitmapSource_Release(s_cover); s_cover = NULL; }
        if (s_wic) { IWICImagingFactory_Release(s_wic); s_wic = NULL; }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HFONT CreateUiFont(HWND hwnd, UINT dpi)
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);

    BOOL ok = FALSE;
    if (g_pSystemParametersInfoForDpi != NULL) {
        ok = g_pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    }
    if (!ok) {
        ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        if (ok) {
            ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)dpi, 96);
        }
    }
    (void)hwnd;

    if (!ok) {
        return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    return (font != NULL) ? font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static int RunGui(HINSTANCE hInstance, int nShowCmd)
{
    InitDpiSupport();

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"HitzLauncherWindow";

    if (RegisterClassExW(&wc) == 0) {
        ShowWin32Error(NULL, L"Could not register the window class.");
        return 1;
    }

    UINT dpi = 96;
    {
        HDC hdc = GetDC(NULL);
        if (hdc != NULL) {
            int d = GetDeviceCaps(hdc, LOGPIXELSX);
            if (d > 0) {
                dpi = (UINT)d;
            }
            ReleaseDC(NULL, hdc);
        }
    }

    int clientW = ScaleDpi(480, dpi);
    int clientH = ScaleDpi(548, dpi);

    RECT rc = { 0, 0, clientW, clientH };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"NHL Hitz Pro", style,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 NULL, NULL, hInstance, NULL);
    if (hwnd == NULL) {
        ShowWin32Error(NULL, L"Could not create the main window.");
        return 1;
    }

    dpi = DpiForWindow(hwnd);
    g_app.font = CreateUiFont(hwnd, dpi);

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_app.font != NULL && g_app.font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(g_app.font);
    }

    return (int)msg.wParam;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    ZeroMemory(&g_app, sizeof(g_app));

    if (!ResolvePaths(&g_app.paths)) {
        ShowWin32Error(NULL, L"Could not determine the NHL Hitz Pro install folder.");
        return 1;
    }

    BOOL printEnv = ParseCommandLine(g_app.paths.ini_path);

    LoadSettings(g_app.paths.ini_path, &g_app.settings);

    if (printEnv) {
        PrintEnvToStdout(&g_app.settings);
        return 0;
    }

    return RunGui(hInstance, nShowCmd);
}
