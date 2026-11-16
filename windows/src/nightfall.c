/*
 * Nightfall - a tiny, low-resource screensaver / screen blanker for Windows.
 *
 *   nightfall.exe            blank the screen now (same as /s)
 *   nightfall.exe /s         blank the screen now (also what Windows passes to a .scr)
 *   nightfall.exe /tray      stay in the notification area:
 *                              - left-click the moon icon  -> blank now
 *                              - Ctrl+Alt+B (global hotkey) -> blank now
 *                              - right-click               -> menu (blank / start with Windows / exit)
 *   nightfall.exe /c         about box (Windows "Settings" button for a .scr)
 *   nightfall.exe /p <hwnd>  preview (ignored, exits)
 *
 * While blanked: one black window covers every monitor, the cursor is hidden,
 * and a dim clock jumps to a random spot every 20 s so nothing is ever burned in.
 * Any key or mouse click dismisses it (mouse movement alone does not, so a bumped
 * mouse won't wake it). CPU use is a 1 Hz timer that usually does nothing.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include "resource.h"

#define APP_NAME        L"Nightfall"
#define TRAY_CLASS      L"NightfallTray"
#define SAVER_CLASS     L"NightfallSaver"
#define RUN_KEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define WM_TRAY         (WM_APP + 1)
#define ID_HOTKEY       1
#define ID_TRAYICON     1
#define IDM_START       100
#define IDM_AUTOSTART   101
#define IDM_EXIT        102
#define TIMER_TICK      1

#define GRACE_MS        800     /* ignore input right after blanking (key release of the hotkey etc.) */
#define MOVE_MS         20000   /* move the clock this often */
#define MAX_MONITORS    16

static HINSTANCE g_inst;
static HWND      g_tray_wnd;
static HWND      g_saver_wnd;
static BOOL      g_saver_only;          /* process exists only to show the saver */
static UINT      g_msg_taskbar_created;
static HICON     g_icon_small;

static DWORD     g_saver_start;
static DWORD     g_last_move;
static int       g_last_minute = -1;
static RECT      g_text_rc;             /* clock block, client coordinates */
static HFONT     g_font_time, g_font_date;
static int       g_time_h;

static RECT      g_monitors[MAX_MONITORS];
static int       g_monitor_count;

/* ---------- small helpers ---------- */

static unsigned rnd(void)
{
    static unsigned s;
    if (!s) s = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x9E3779B9u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static BOOL CALLBACK monitor_enum(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp)
{
    (void)m; (void)dc; (void)lp;
    if (g_monitor_count < MAX_MONITORS) g_monitors[g_monitor_count++] = *rc;
    return TRUE;
}

static void format_clock(wchar_t *time_buf, int tn, wchar_t *date_buf, int dn, SYSTEMTIME *st)
{
    GetLocalTime(st);
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, st, NULL, time_buf, tn))
        wsprintfW(time_buf, L"%02d:%02d", st->wHour, st->wMinute);
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, st, NULL, date_buf, dn, NULL))
        date_buf[0] = 0;
}

static void make_fonts(int monitor_h)
{
    if (g_font_time) DeleteObject(g_font_time);
    if (g_font_date) DeleteObject(g_font_date);
    g_time_h = monitor_h / 7;
    if (g_time_h < 24) g_time_h = 24;
    g_font_time = CreateFontW(-g_time_h, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI Light");
    g_font_date = CreateFontW(-(g_time_h / 4), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
}

/* Pick a random monitor and a random spot on it for the clock block. */
static void place_clock(HWND hwnd)
{
    wchar_t tbuf[64], dbuf[128];
    SYSTEMTIME st;
    SIZE ts = {0}, ds = {0};
    RECT mon;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w, h, x, y, mw, mh;
    HDC dc;

    if (g_monitor_count == 0) {
        g_monitors[0].left = vx; g_monitors[0].top = vy;
        g_monitors[0].right = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_monitors[0].bottom = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_monitor_count = 1;
    }
    mon = g_monitors[rnd() % (unsigned)g_monitor_count];
    mw = mon.right - mon.left;
    mh = mon.bottom - mon.top;
    make_fonts(mh);

    format_clock(tbuf, 64, dbuf, 128, &st);
    dc = GetDC(hwnd);
    SelectObject(dc, g_font_time);
    GetTextExtentPoint32W(dc, tbuf, lstrlenW(tbuf), &ts);
    SelectObject(dc, g_font_date);
    GetTextExtentPoint32W(dc, dbuf, lstrlenW(dbuf), &ds);
    ReleaseDC(hwnd, dc);

    /* leave room for wider strings later (e.g. 9:59 -> 10:00) */
    w = (ts.cx > ds.cx ? ts.cx : ds.cx) + g_time_h;
    h = ts.cy + ds.cy;
    x = mon.left + (mw > w ? (int)(rnd() % (unsigned)(mw - w)) : 0);
    y = mon.top  + (mh > h ? (int)(rnd() % (unsigned)(mh - h)) : 0);

    g_text_rc.left = x - vx;
    g_text_rc.top = y - vy;
    g_text_rc.right = g_text_rc.left + w;
    g_text_rc.bottom = g_text_rc.top + h;
}

/* ---------- saver window ---------- */

static void saver_dismiss(HWND hwnd)
{
    if (GetTickCount() - g_saver_start < GRACE_MS) return;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK saver_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_saver_start = g_last_move = GetTickCount();
        g_monitor_count = 0;
        EnumDisplayMonitors(NULL, NULL, monitor_enum, 0);
        place_clock(hwnd);
        SetTimer(hwnd, TIMER_TICK, 1000, NULL);
        return 0;

    case WM_TIMER: {
        SYSTEMTIME st;
        DWORD now = GetTickCount();
        GetLocalTime(&st);
        if (now - g_last_move >= MOVE_MS) {
            g_last_move = now;
            InvalidateRect(hwnd, &g_text_rc, TRUE);    /* erase old spot */
            place_clock(hwnd);
            InvalidateRect(hwnd, &g_text_rc, TRUE);    /* paint new spot */
        } else if (st.wMinute != g_last_minute) {
            InvalidateRect(hwnd, &g_text_rc, TRUE);
        }
        /* keep us on top of anything that popped up */
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        wchar_t tbuf[64], dbuf[128];
        SYSTEMTIME st;
        RECT r;
        HDC dc = BeginPaint(hwnd, &ps);
        format_clock(tbuf, 64, dbuf, 128, &st);
        g_last_minute = st.wMinute;
        SetBkMode(dc, TRANSPARENT);
        r = g_text_rc;
        SelectObject(dc, g_font_time);
        SetTextColor(dc, RGB(78, 78, 84));
        DrawTextW(dc, tbuf, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        r.top += g_time_h + g_time_h / 3;
        SelectObject(dc, g_font_date);
        SetTextColor(dc, RGB(52, 52, 58));
        DrawTextW(dc, dbuf, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (lp & (1 << 30)) return 0;      /* auto-repeat of a key held since before we appeared */
        saver_dismiss(hwnd);
        return 0;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        saver_dismiss(hwnd);
        return 0;

    case WM_SYSCOMMAND:
        /* swallow the system's own screensaver while we're up; let everything else through */
        if ((wp & 0xFFF0) == SC_SCREENSAVE) return 0;
        break;

    case WM_DISPLAYCHANGE:
        SetWindowPos(hwnd, HWND_TOPMOST,
                     GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), 0);
        g_monitor_count = 0;
        EnumDisplayMonitors(NULL, NULL, monitor_enum, 0);
        place_clock(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        if (g_font_time) { DeleteObject(g_font_time); g_font_time = NULL; }
        if (g_font_date) { DeleteObject(g_font_date); g_font_date = NULL; }
        g_saver_wnd = NULL;
        if (g_saver_only) PostQuitMessage(0);
        else SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); /* give memory back */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void saver_start(void)
{
    if (g_saver_wnd) { SetForegroundWindow(g_saver_wnd); return; }
    g_saver_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SAVER_CLASS, APP_NAME,
                                  WS_POPUP,
                                  GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                                  GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                                  NULL, NULL, g_inst, NULL);
    if (!g_saver_wnd) return;
    ShowWindow(g_saver_wnd, SW_SHOW);
    SetForegroundWindow(g_saver_wnd);
    SetFocus(g_saver_wnd);
}

/* ---------- tray ---------- */

static BOOL autostart_get(void)
{
    HKEY k;
    BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void autostart_set(BOOL on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH], cmd[MAX_PATH + 16];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wsprintfW(cmd, L"\"%s\" /tray", path);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((lstrlenW(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

static void tray_add(HWND hwnd, BOOL hotkey_ok)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon_small;
    lstrcpynW(nid.szTip, hotkey_ok ? L"Nightfall - click or Ctrl+Alt+B to blank the screen"
                                   : L"Nightfall - click to blank the screen", 128);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void tray_remove(HWND hwnd)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void tray_menu(HWND hwnd)
{
    POINT pt;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_START, L"&Blank screen now\tCtrl+Alt+B");
    AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with &Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetMenuDefaultItem(m, IDM_START, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static BOOL g_hotkey_ok;

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msg_taskbar_created && g_msg_taskbar_created) {   /* explorer restarted */
        tray_add(hwnd, g_hotkey_ok);
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        g_hotkey_ok = RegisterHotKey(hwnd, ID_HOTKEY, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'B');
        tray_add(hwnd, g_hotkey_ok);
        return 0;
    case WM_HOTKEY:
        if (wp == ID_HOTKEY) saver_start();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:   saver_start(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: tray_menu(hwnd); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_START:     saver_start(); break;
        case IDM_AUTOSTART: autostart_set(!autostart_get()); break;
        case IDM_EXIT:      DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        if (g_saver_wnd) DestroyWindow(g_saver_wnd);
        UnregisterHotKey(hwnd, ID_HOTKEY);
        tray_remove(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- entry ---------- */

static BOOL arg_is(const wchar_t *a, wchar_t c)
{
    return a && (a[0] == L'/' || a[0] == L'-') &&
           (a[1] == c || a[1] == (wchar_t)(c - 32)) &&
           (a[2] == 0 || a[2] == L':' || a[2] == L' ');
}

static BOOL arg_word(const wchar_t *a, const wchar_t *w)
{
    return a && (a[0] == L'/' || a[0] == L'-') && lstrcmpiW(a + 1, w) == 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *a1 = argc > 1 ? argv[1] : NULL;
    WNDCLASSEXW wc;
    MSG msg;
    (void)prev; (void)cmdline; (void)show;

    g_inst = inst;
    g_icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    if (arg_is(a1, L'p')) return 0;                               /* preview: nothing to show */
    if (arg_is(a1, L'c')) {
        MessageBoxW(NULL,
            L"Nightfall blanks every screen with a dim, wandering clock.\n\n"
            L"Dismiss: any key or mouse click (moving the mouse does not wake it).\n"
            L"Tray mode: run  nightfall.exe /tray  - click the moon or press Ctrl+Alt+B.",
            APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = g_icon_small;

    wc.lpfnWndProc = saver_proc;
    wc.lpszClassName = SAVER_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    if (arg_word(a1, L"tray")) {
        HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\NightfallTraySingleton");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;     /* already in the tray */
        g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        wc.lpfnWndProc = tray_proc;
        wc.lpszClassName = TRAY_CLASS;
        wc.hbrBackground = NULL;
        wc.style = 0;
        RegisterClassExW(&wc);
        g_tray_wnd = CreateWindowExW(0, TRAY_CLASS, APP_NAME, WS_OVERLAPPED, 0, 0, 0, 0,
                                     NULL, NULL, inst, NULL);
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        (void)mtx;
        return 0;
    }

    /* default and /s: blank now, exit when dismissed */
    g_saver_only = TRUE;
    saver_start();
    if (!g_saver_wnd) return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    LocalFree(argv);
    return 0;
}
