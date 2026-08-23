// capslang — перемикання розкладки клавіатури по CapsLock (Windows 11).
//
// Механізм: RegisterHotKey(VK_CAPITAL) — система проковтує CapsLock до того,
// як його побачить будь-який застосунок; реєстрація ексклюзивна і не
// "відвалюється" з часом (на відміну від low-level hook).
// Shift+CapsLock не матчиться хоткеєм без модифікаторів → лишається
// звичайним перемикачем регістру.
//
// Маніфест requireAdministrator: без нього UIPI блокує
// WM_INPUTLANGCHANGEREQUEST у бік elevated-вікон (адмінський термінал тощо).
// Автозапуск — задача Task Scheduler з RL HIGHEST (Run-ключ реєстру для
// elevated-програм Windows ігнорує, а задача стартує без UAC-промпта).

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>

namespace {

constexpr UINT WMAPP_TRAY         = WM_APP + 1;
constexpr UINT WMAPP_SHOWSETTINGS = WM_APP + 2;
constexpr int  HOTKEY_ID     = 1;
constexpr int  IDC_AUTOSTART = 100;
constexpr UINT IDM_SETTINGS  = 1;
constexpr UINT IDM_EXIT      = 2;

const wchar_t* kWndClass = L"capslang";
const wchar_t* kTaskName = L"capslang";

NOTIFYICONDATAW g_nid = {};
HWND g_checkbox = nullptr;
UINT g_taskbarCreatedMsg = 0;

// ---------- перемикання розкладки ----------

// Реально сфокусоване вікно (для UWP foreground != focus)
HWND GetFocusedWindow()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;

    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus)
        return gti.hwndFocus;
    return fg;
}

HKL NextLayout(HWND target)
{
    HKL list[16];
    UINT n = GetKeyboardLayoutList(16, list);
    if (n < 2) return nullptr;

    DWORD tid = target ? GetWindowThreadProcessId(target, nullptr) : 0;
    HKL cur = GetKeyboardLayout(tid);

    for (UINT i = 0; i < n; ++i)
        if (list[i] == cur)
            return list[(i + 1) % n];
    return list[0];
}

void SwitchLayout()
{
    HWND target = GetFocusedWindow();
    HKL next = NextLayout(target);
    if (!next) return;

    if (target)
        PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)next);
    else
        ActivateKeyboardLayout(next, 0);
}

// ---------- автозапуск (Task Scheduler) ----------

DWORD RunHidden(wchar_t* cmdline)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

bool AutostartEnabled()
{
    wchar_t cmd[256];
    wsprintfW(cmd, L"schtasks /Query /TN \"%s\"", kTaskName);
    return RunHidden(cmd) == 0;
}

bool SetAutostart(bool enable)
{
    wchar_t cmd[1024];
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wsprintfW(cmd,
            L"schtasks /Create /F /TN \"%s\" /TR \"\\\"%s\\\"\" /SC ONLOGON /RL HIGHEST",
            kTaskName, exe);
    } else {
        wsprintfW(cmd, L"schtasks /Delete /F /TN \"%s\"", kTaskName);
    }
    RunHidden(cmd);
    return AutostartEnabled() == enable;
}

// ---------- GUI ----------

void ShowSettings(HWND hwnd)
{
    SendMessageW(g_checkbox, BM_SETCHECK,
                 AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Налаштування…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Вихід");
    SetForegroundWindow(hwnd); // інакше меню не закриється кліком повз
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        // Explorer перезапустився — повертаємо іконку в трей
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) SwitchLayout();
        return 0;

    case WMAPP_SHOWSETTINGS:
        ShowSettings(hwnd);
        return 0;

    case WMAPP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            ShowSettings(hwnd);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hwnd);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_AUTOSTART:
            if (HIWORD(wp) == BN_CLICKED) {
                bool want = SendMessageW(g_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (!SetAutostart(want))
                    MessageBoxW(hwnd,
                        L"Не вдалося змінити задачу автозапуску (schtasks).",
                        L"capslang", MB_ICONERROR | MB_OK);
                SendMessageW(g_checkbox, BM_SETCHECK,
                             AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            return 0;
        case IDM_SETTINGS:
            ShowSettings(hwnd);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE); // закриття вікна не завершує програму
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HFONT CreateUIFont()
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    CreateMutexW(nullptr, TRUE, L"capslang_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Другий запуск — показуємо вікно першого екземпляра
        if (HWND prev = FindWindowW(kWndClass, nullptr))
            PostMessageW(prev, WMAPP_SHOWSETTINGS, 0, 0);
        return 0;
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    const UINT dpi = GetDpiForSystem();
    auto sc = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWndClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    const int w = sc(400), h = sc(140);
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND hwnd = CreateWindowW(kWndClass, L"capslang", WS_CAPTION | WS_SYSMENU,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    HFONT font = CreateUIFont();
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                  int x, int y, int cx, int cy, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style,
                               sc(x), sc(y), sc(cx), sc(cy),
                               hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    mk(L"STATIC", L"CapsLock — перемкнути розкладку", 0, 20, 16, 360, 20, 0);
    mk(L"STATIC", L"Shift + CapsLock — звичайний Caps Lock", 0, 20, 40, 360, 20, 0);
    g_checkbox = mk(L"BUTTON", L"Запускати при вході в Windows",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 20, 80, 360, 24, IDC_AUTOSTART);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = hwnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WMAPP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpyW(g_nid.szTip, L"capslang — CapsLock перемикає розкладку");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!RegisterHotKey(hwnd, HOTKEY_ID, MOD_NOREPEAT, VK_CAPITAL)) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        MessageBoxW(nullptr,
            L"Не вдалося зареєструвати CapsLock:\n"
            L"клавішу вже перехопила інша програма.",
            L"capslang", MB_ICONERROR | MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) { // Tab/Space у вікні налаштувань
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    UnregisterHotKey(hwnd, HOTKEY_ID);
    return 0;
}
