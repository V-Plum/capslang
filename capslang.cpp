// capslang — перемикання розкладки клавіатури по CapsLock (Windows 11).
//
// Механізм: low-level клавіатурний хук, який ковтає CapsLock (повертає 1) і
// віддає роботу головному потоку. RegisterHotKey тут НЕ підходить, хоч і
// виглядає охайніше: він перехоплює доставку повідомлення, але сам тогл
// Caps Lock відбувається рівнем нижче й однаково спрацьовує, тобто після
// кожного непарного перемикання розкладки лишався ввімкнений капс. Скасувати
// той тогл ін'єкцією CapsLock теж не вийде — власну ін'єкцію з'їдає власна ж
// реєстрація хоткея (перевірено: SendInput проходить, WM_HOTKEY не приходить,
// стан не змінюється). Хук — єдиний спосіб не дати капсу перемкнутися.
//
// Shift+CapsLock хук пропускає далі → лишається звичайним Caps Lock.
//
// Ціна хука: Windows знімає його, якщо колбек не встигає за
// LowLevelHooksTimeout. Тому в колбеку немає нічого, крім перевірки клавіші й
// PostMessage — уся робота (перелік розкладок, надсилання повідомлень чужим
// вікнам) виконується вже в циклі повідомлень.
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
#include <shlwapi.h>
#include <commctrl.h>
#include <taskschd.h>
#include <gdiplus.h>

namespace {

constexpr UINT WMAPP_TRAY         = WM_APP + 1;
constexpr UINT WMAPP_SHOWSETTINGS = WM_APP + 2;
constexpr UINT WMAPP_SWITCH       = WM_APP + 3;
constexpr int  IDC_AUTOSTART   = 100;
constexpr int  IDC_COPYRIGHT   = 101;
constexpr int  IDC_MODE_HOOK   = 102;
constexpr int  IDC_MODE_HOTKEY = 103;
constexpr int  IDC_MODE_HINT   = 104;
constexpr int  IDR_LOGO_PNG    = 100;  // RCDATA з capslang.png
constexpr int  HOTKEY_ID       = 1;
constexpr UINT IDM_SETTINGS    = 1;
constexpr UINT IDM_EXIT        = 2;

const wchar_t* kWndClass = L"capslang";
const wchar_t* kTaskName = L"capslang";
const wchar_t* kRegPath  = L"Software\\capslang";
const wchar_t* kRegMode  = L"Mode";

// Два способи перехопити клавішу. Основний тримає Caps Lock вимкненим, але це
// клавіатурний хук, який деякі захисні програми не люблять; запасний працює
// через системну реєстрацію клавіші й нічого не перехоплює, але тоді Windows
// сама перемикає Caps Lock — див. коментар на початку файлу.
enum class Mode { Hook = 0, Hotkey = 1 };

NOTIFYICONDATAW g_nid = {};
HWND g_checkbox = nullptr;
HWND g_modeHint = nullptr;
UINT g_taskbarCreatedMsg = 0;
Mode g_mode = Mode::Hook;

ULONG_PTR g_gdiplusToken = 0;
Gdiplus::Image* g_logo = nullptr;
RECT g_logoRect = {};  // куди малювати логотип (пікселі клієнтської області)

HHOOK g_hook = nullptr;
HWND  g_mainWnd = nullptr;
bool  g_capsDown = false;  // щоб автоповтор не перемикав розкладку нескінченно

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

// ---------- перехоплення клавіші ----------
//
// Колбек свідомо мінімальний: усе, що складніше за PostMessage, ризикує не
// вкластися в LowLevelHooksTimeout, після чого Windows тихо зніме хук — і
// утиліта "просто перестане працювати" без жодної помилки.
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
    if (k->vkCode != VK_CAPITAL)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    // Shift+CapsLock лишається справжнім Caps Lock — пропускаємо як є
    if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000)) {
        g_capsDown = false;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        if (!g_capsDown) {
            g_capsDown = true;
            PostMessageW(g_mainWnd, WMAPP_SWITCH, 0, 0);
        }
        return 1;  // саме це не дає перемкнутися регістру
    }

    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        g_capsDown = false;
        return 1;
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ---------- режим роботи ----------

void StopInterception()
{
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    UnregisterHotKey(g_mainWnd, HOTKEY_ID);
    g_capsDown = false;
}

bool StartInterception(Mode mode)
{
    if (mode == Mode::Hook) {
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                   GetModuleHandleW(nullptr), 0);
        return g_hook != nullptr;
    }
    return RegisterHotKey(g_mainWnd, HOTKEY_ID, MOD_NOREPEAT, VK_CAPITAL) != FALSE;
}

Mode LoadMode()
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegMode, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS && value == 1)
        return Mode::Hotkey;
    return Mode::Hook;
}

void SaveMode(Mode mode)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD value = (mode == Mode::Hotkey) ? 1 : 0;
    RegSetValueExW(key, kRegMode, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(key);
}

// ---------- автозапуск (Task Scheduler через COM) ----------
//
// Свідомо НЕ через запуск schtasks.exe: породження дочірнього процесу, який
// створює задачу з найвищими правами, — типовий персистенс-патерн малварі, і
// ML-евристики антивірусів на нього реагують. COM-шлях робить те саме напряму.

// Підключення до планувальника; при true — звільнити обидва вказівники.
bool OpenTaskRoot(ITaskService** svcOut, ITaskFolder** rootOut)
{
    *svcOut = nullptr;
    *rootOut = nullptr;

    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void**)&svc)))
        return false;

    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(svc->Connect(empty, empty, empty, empty))) {
        svc->Release();
        return false;
    }

    ITaskFolder* root = nullptr;
    BSTR path = SysAllocString(L"\\");
    HRESULT hr = svc->GetFolder(path, &root);
    SysFreeString(path);
    if (FAILED(hr)) {
        svc->Release();
        return false;
    }

    *svcOut = svc;
    *rootOut = root;
    return true;
}

bool AutostartEnabled()
{
    ITaskService* svc;
    ITaskFolder* root;
    if (!OpenTaskRoot(&svc, &root)) return false;

    IRegisteredTask* task = nullptr;
    BSTR name = SysAllocString(kTaskName);
    bool found = SUCCEEDED(root->GetTask(name, &task)) && task;
    SysFreeString(name);

    if (task) task->Release();
    root->Release();
    svc->Release();
    return found;
}

void FillTaskDefinition(ITaskDefinition* def)
{
    IRegistrationInfo* info = nullptr;
    if (SUCCEEDED(def->get_RegistrationInfo(&info)) && info) {
        BSTR s = SysAllocString(L"capslang — CapsLock перемикає розкладку клавіатури");
        info->put_Description(s);
        SysFreeString(s);
        info->Release();
    }

    // Найвищі права: без них перемикання не діє в elevated-вікнах (UIPI)
    IPrincipal* principal = nullptr;
    if (SUCCEEDED(def->get_Principal(&principal)) && principal) {
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->Release();
    }

    // Дефолти планувальника розраховані на разові задачі й фоновому застосунку
    // шкідливі: на батареї він би не стартував, а через 3 доби безперервної
    // роботи його вбило б по ExecutionTimeLimit.
    ITaskSettings* settings = nullptr;
    if (SUCCEEDED(def->get_Settings(&settings)) && settings) {
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        BSTR noLimit = SysAllocString(L"PT0S");
        settings->put_ExecutionTimeLimit(noLimit);
        SysFreeString(noLimit);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);

        IIdleSettings* idle = nullptr;
        if (SUCCEEDED(settings->get_IdleSettings(&idle)) && idle) {
            idle->put_StopOnIdleEnd(VARIANT_FALSE);
            idle->Release();
        }
        settings->Release();
    }

    ITriggerCollection* triggers = nullptr;
    if (SUCCEEDED(def->get_Triggers(&triggers)) && triggers) {
        ITrigger* trigger = nullptr;
        if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger)
            trigger->Release();
        triggers->Release();
    }

    IActionCollection* actions = nullptr;
    if (SUCCEEDED(def->get_Actions(&actions)) && actions) {
        IAction* action = nullptr;
        if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
            IExecAction* exec = nullptr;
            if (SUCCEEDED(action->QueryInterface(IID_IExecAction, (void**)&exec)) && exec) {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                BSTR p = SysAllocString(exePath);
                exec->put_Path(p);
                SysFreeString(p);
                exec->Release();
            }
            action->Release();
        }
        actions->Release();
    }
}

bool SetAutostart(bool enable)
{
    ITaskService* svc;
    ITaskFolder* root;
    if (!OpenTaskRoot(&svc, &root)) return false;

    BSTR name = SysAllocString(kTaskName);
    bool ok = false;

    if (!enable) {
        HRESULT hr = root->DeleteTask(name, 0);
        ok = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    } else {
        ITaskDefinition* def = nullptr;
        if (SUCCEEDED(svc->NewTask(0, &def)) && def) {
            FillTaskDefinition(def);

            VARIANT empty;
            VariantInit(&empty);
            IRegisteredTask* registered = nullptr;
            ok = SUCCEEDED(root->RegisterTaskDefinition(
                name, def, TASK_CREATE_OR_UPDATE,
                empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered));
            if (registered) registered->Release();
            def->Release();
        }
    }

    SysFreeString(name);
    root->Release();
    svc->Release();
    return ok;
}

// ---------- GUI ----------

// PNG-логотип із ресурсів (GDI+ малює його з альфа-каналом поверх фону вікна)
void LoadLogo(HINSTANCE hInst)
{
    HRSRC res = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_LOGO_PNG), RT_RCDATA);
    if (!res) return;
    HGLOBAL blob = LoadResource(hInst, res);
    void* data = LockResource(blob);
    DWORD size = SizeofResource(hInst, res);
    if (!data || !size) return;

    if (IStream* stream = SHCreateMemStream((const BYTE*)data, size)) {
        g_logo = Gdiplus::Image::FromStream(stream);
        stream->Release();
        if (g_logo && g_logo->GetLastStatus() != Gdiplus::Ok) {
            delete g_logo;
            g_logo = nullptr;
        }
    }
}

void PaintWindow(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (g_logo) {
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(g_logo, (INT)g_logoRect.left, (INT)g_logoRect.top,
                    (INT)(g_logoRect.right - g_logoRect.left),
                    (INT)(g_logoRect.bottom - g_logoRect.top));
    }
    EndPaint(hwnd, &ps);
}

void ShowSettings(HWND hwnd)
{
    SendMessageW(g_checkbox, BM_SETCHECK,
                 AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void UpdateModeHint()
{
    SetWindowTextW(g_modeHint, g_mode == Mode::Hook
        ? L"CapsLock лише перемикає мову й не вмикає великі літери."
        : L"Оберіть, якщо основний режим не працює або конфліктує з іншою програмою.");
}

// Перемикання режиму наживо: знімаємо поточний перехоплювач і ставимо інший.
void ApplyMode(HWND hwnd, Mode mode)
{
    StopInterception();
    if (!StartInterception(mode)) {
        // не вийшло — вертаємось на те, що працювало
        if (StartInterception(g_mode)) {
            MessageBoxW(hwnd, L"Цей режим зараз недоступний — залишено попередній.",
                        L"capslang", MB_ICONWARNING | MB_OK);
        } else {
            MessageBoxW(hwnd, L"Не вдалося перехопити клавішу CapsLock.",
                        L"capslang", MB_ICONERROR | MB_OK);
        }
    } else {
        g_mode = mode;
        SaveMode(mode);
    }

    CheckRadioButton(hwnd, IDC_MODE_HOOK, IDC_MODE_HOTKEY,
                     g_mode == Mode::Hook ? IDC_MODE_HOOK : IDC_MODE_HOTKEY);
    UpdateModeHint();
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
    case WMAPP_SWITCH:   // від хука
    case WM_HOTKEY:      // від системної реєстрації клавіші
        SwitchLayout();
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
        case IDC_MODE_HOOK:
            if (HIWORD(wp) == BN_CLICKED && g_mode != Mode::Hook)
                ApplyMode(hwnd, Mode::Hook);
            return 0;
        case IDC_MODE_HOTKEY:
            if (HIWORD(wp) == BN_CLICKED && g_mode != Mode::Hotkey)
                ApplyMode(hwnd, Mode::Hotkey);
            return 0;
        case IDM_SETTINGS:
            ShowSettings(hwnd);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if (GetDlgCtrlID((HWND)lp) == IDC_COPYRIGHT)
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
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

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdipInput, nullptr);
    LoadLogo(hInst);

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

    const int w = sc(460), h = sc(246);
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
    mk(L"STATIC", L"CapsLock — перемкнути розкладку", 0, 20, 16, 300, 20, 0);
    mk(L"STATIC", L"Shift + CapsLock — звичайний Caps Lock", 0, 20, 40, 300, 20, 0);
    g_checkbox = mk(L"BUTTON", L"Запускати при вході в Windows",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 20, 74, 300, 24, IDC_AUTOSTART);

    mk(L"STATIC", L"Режим роботи:", 0, 20, 118, 200, 20, 0);
    mk(L"BUTTON", L"Основний", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
       20, 142, 130, 22, IDC_MODE_HOOK);
    mk(L"BUTTON", L"Запасний", BS_AUTORADIOBUTTON,
       160, 142, 130, 22, IDC_MODE_HOTKEY);
    g_modeHint = mk(L"STATIC", L"", 0, 20, 170, 420, 20, IDC_MODE_HINT);

    mk(L"STATIC", L"© Plum, 2026", 0, 20, 210, 200, 18, IDC_COPYRIGHT);

    // Логотип праворуч від налаштувань
    SetRect(&g_logoRect, sc(354), sc(22), sc(354 + 84), sc(22 + 84));

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

    g_mainWnd = hwnd;
    g_mode = LoadMode();
    if (!StartInterception(g_mode)) {
        // збережений режим не піднявся — пробуємо інший, щоб утиліта не була мертвою
        Mode other = (g_mode == Mode::Hook) ? Mode::Hotkey : Mode::Hook;
        if (!StartInterception(other)) {
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            MessageBoxW(nullptr, L"Не вдалося перехопити клавішу CapsLock.",
                        L"capslang", MB_ICONERROR | MB_OK);
            return 1;
        }
        g_mode = other;
    }
    CheckRadioButton(hwnd, IDC_MODE_HOOK, IDC_MODE_HOTKEY,
                     g_mode == Mode::Hook ? IDC_MODE_HOOK : IDC_MODE_HOTKEY);
    UpdateModeHint();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) { // Tab/Space у вікні налаштувань
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    StopInterception();
    delete g_logo;
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}
