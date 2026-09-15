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
// LowLevelHooksTimeout (~300 мс). Тому, по-перше, колбек не робить нічого, крім
// перевірки клавіші й PostMessage; по-друге, хук живе на ОКРЕМОМУ потоці з
// власним циклом повідомлень — щоб зайнятість UI-потоку (наприклад, синхронні
// COM-виклики планувальника при вмиканні автозапуску) не могла задушити колбек.
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
// MinGW затягує їх транзитивно, MSVC — ні: sqrt() у детекторі жесту й _wtoi()
// у полях «Детально» інакше валять саме релізну збірку, а не локальну.
#include <math.h>
#include <stdlib.h>
// CAPS-7: день/ніч — геолокація за IP (WinINet), Location API (COM), час, форматування.
#include <wininet.h>
#include <locationapi.h>   // лише інтерфейси; GUID-и нижче свої — SDK MSVC тримає їх у locationapi.lib,
                           // MinGW — у заголовку, і сходяться вони лише через власні копії
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

namespace {

constexpr UINT WMAPP_TRAY         = WM_APP + 1;
constexpr UINT WMAPP_SHOWSETTINGS = WM_APP + 2;
constexpr UINT WMAPP_SWITCH       = WM_APP + 3;
constexpr UINT WMAPP_SHAKE        = WM_APP + 4;   // від мишачого хука: жест розпізнано
constexpr UINT WMAPP_MAGDONE      = WM_APP + 5;   // потік анімації: зменшення завершено
constexpr UINT WMAPP_THEMELOC     = WM_APP + 6;   // потік геолокації: lp = LocResult* (heap)
constexpr UINT HKW_INSTALL        = WM_APP + 20;  // до вікна потоку хука
constexpr UINT HKW_UNINSTALL      = WM_APP + 21;
constexpr UINT HKW_MOUSE_ON       = WM_APP + 22;
constexpr UINT HKW_MOUSE_OFF      = WM_APP + 23;
constexpr int  IDC_AUTOSTART   = 100;
constexpr int  IDC_COPYRIGHT   = 101;
constexpr int  IDC_MODE_HOOK   = 102;
constexpr int  IDC_MODE_HOTKEY = 103;
constexpr int  IDC_MODE_HINT   = 104;
constexpr int  IDC_PASSTHROUGH      = 105;
constexpr int  IDC_PASSTHROUGH_HINT = 106;
// CAPS-2: вкладка «Курсор»
constexpr int  IDC_TABS          = 110;
constexpr int  IDC_CUR_ENABLE    = 111;
constexpr int  IDC_CUR_SCALE     = 112;
constexpr int  IDC_CUR_HOLD      = 113;
constexpr int  IDC_CUR_ADVANCED  = 114;
constexpr int  IDC_CUR_WINDOWMS  = 115;
constexpr int  IDC_CUR_DIST      = 116;
constexpr int  IDC_CUR_FACTOR    = 117;
constexpr int  IDC_CUR_REVERSALS = 118;
constexpr int  IDC_CUR_SHRINK    = 119;
constexpr int  IDC_CUR_OVERLAY   = 121;
constexpr int  IDC_HINT_GRAY     = 120;  // будь-який сірий пояснювальний текст
// CAPS-7: вкладка «День/ніч»
constexpr int  IDC_TH_ENABLE     = 130;
constexpr int  IDC_TH_BY_SUN     = 131;
constexpr int  IDC_TH_BY_SCHED   = 132;
constexpr int  IDC_TH_DARK_FROM  = 133;
constexpr int  IDC_TH_LIGHT_FROM = 134;
constexpr int  IDC_TH_TOGGLE     = 135;
constexpr int  IDC_TH_ADVANCED   = 136;
constexpr int  IDC_TH_SRC_AUTO   = 137;  // порядок = LocSource
constexpr int  IDC_TH_SRC_WIN    = 138;
constexpr int  IDC_TH_SRC_IP     = 139;
constexpr int  IDC_TH_SRC_MANUAL = 140;
constexpr int  IDC_TH_SRC_TZ     = 141;
constexpr int  IDC_TH_LAT        = 142;
constexpr int  IDC_TH_LON        = 143;
constexpr int  IDC_TH_STATUS     = 144;
constexpr int  IDC_TH_NOW        = 145;
constexpr int  IDR_LOGO_PNG    = 100;  // RCDATA з capslang.png
constexpr int  HOTKEY_ID       = 1;
constexpr UINT IDM_SETTINGS    = 1;
constexpr UINT IDM_EXIT        = 2;
constexpr UINT TIMER_MAG_HOLD   = 1;
constexpr UINT TIMER_MAG_FRAME  = 2;   // кадр оверлейної анімації
constexpr UINT TIMER_THEME      = 3;   // CAPS-7: перевірка теми раз на хвилину

const wchar_t* kWndClass = L"capslang";
const wchar_t* kTaskName = L"capslang";
const wchar_t* kRegPath  = L"Software\\capslang";
const wchar_t* kRegMode  = L"Mode";
const wchar_t* kRegPassthrough = L"PassthroughRemote";

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

// CAPS-1: не перехоплювати Caps у вікнах віддалених/віртуальних машин.
volatile bool g_passthrough = true;   // налаштування (чекбокс), збереж. у реєстрі
volatile bool g_inRemote    = false;  // активне вікно — remote/VM (оновлює WinEvent)
bool  g_interceptionOn = false;       // перехоплення активне (для Hotkey-контексту)
bool  g_hotkeyActive   = false;       // RegisterHotKey зараз тримається
HWINEVENTHOOK g_winEvent = nullptr;
HWND  g_passthroughCheckbox = nullptr;

ULONG_PTR g_gdiplusToken = 0;
Gdiplus::Image* g_logo = nullptr;
RECT g_logoRect = {};  // куди малювати логотип (пікселі клієнтської області)

// CAPS-2: вкладки. Сторінки — звичайні діти головного вікна поверх таб-контрола
// (створені ПІСЛЯ нього, тож лежать вище за z-order); перемикання = show/hide.
HWND g_tabs = nullptr;
HWND g_pageLayout[16] = {};  int g_pageLayoutN = 0;
HWND g_pageCursor[24] = {};  int g_pageCursorN = 0;
HWND g_advCtrls[16]   = {};  int g_advN = 0;
HWND g_curEnable = nullptr, g_curScale = nullptr, g_curHold = nullptr;
HWND g_curOverlay = nullptr;
HWND g_curScaleVal = nullptr, g_curHoldVal = nullptr, g_curAdvBtn = nullptr;
HWND g_edWindow = nullptr, g_edDist = nullptr, g_edFactor = nullptr;
HWND g_edRevers = nullptr, g_edShrink = nullptr;
bool g_advVisible = false;

HHOOK  g_hook = nullptr;
HHOOK  g_mouseHook = nullptr;
HWND   g_mainWnd = nullptr;
bool   g_capsDown = false;  // щоб автоповтор не перемикав розкладку нескінченно

// ---------- CAPS-7: день/ніч — автоматична світла/темна тема Windows ----------
//
// Тема — два DWORD у HKCU\...\Themes\Personalize (AppsUseLightTheme,
// SystemUsesLightTheme; 0 = темна) + бродкаст WM_SETTINGCHANGE "ImmersiveColorSet",
// без якого частина вікон не перемальовується. Перемикаємо обидва разом (рішення
// власника: менше мішанини). Момент — за сходом/заходом сонця (NOAA) або за
// розкладом. Розташування: служба геолокації Windows → за IP → часовий пояс і
// регіон Windows, або вручну; результат кешується в реєстрі, щоб після старту не
// чекати сенсора чи мережі. «Переключити зараз» — ручний вибір ДО НАСТУПНОЇ МЕЖІ
// (наступного сходу/заходу або часу розкладу), далі автоматика знову рахує стан
// від розкладу, а не просто фліпає. Поки на передньому плані повноекранна
// програма, тему не чіпаємо — перемкнемо, щойно вона закриється.
const wchar_t* kRegThemeAuto      = L"ThemeAuto";
const wchar_t* kRegThemeSched     = L"ThemeBySchedule";
const wchar_t* kRegThemeDarkFrom  = L"ThemeDarkFromMin";
const wchar_t* kRegThemeLightFrom = L"ThemeLightFromMin";
const wchar_t* kRegThemeLocSrc    = L"ThemeLocationSource";
const wchar_t* kRegThemeLat       = L"ThemeLatitude";        // ручні координати, REG_SZ
const wchar_t* kRegThemeLon       = L"ThemeLongitude";
const wchar_t* kRegThemeCacheLat  = L"ThemeCacheLatitude";   // останнє визначене розташування
const wchar_t* kRegThemeCacheLon  = L"ThemeCacheLongitude";
const wchar_t* kRegThemeCacheSrc  = L"ThemeCacheSource";
const wchar_t* kRegThemeCacheAt   = L"ThemeCacheAt";         // unix-час (DWORD)
const wchar_t* kRegThemeOvUntil   = L"ThemeOverrideUntil";   // ручний вибір діє до (unix)
const wchar_t* kRegThemeOvDark    = L"ThemeOverrideDark";
const wchar_t* kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

enum class LocSource { Auto = 0, Windows = 1, Ip = 2, Manual = 3, TimeZone = 4 };

// Location API (Win32, COM). Власні копії GUID-ів — див. коментар біля #include.
const GUID kCLSID_Location     = { 0xe5b8e079, 0xee6d, 0x4e33, { 0xa4, 0x38, 0xc8, 0x7f, 0x2e, 0x95, 0x92, 0x54 } };
const GUID kIID_ILocation      = { 0xab2ece69, 0x56d9, 0x4f28, { 0xb5, 0x25, 0xde, 0x1b, 0x0e, 0xe4, 0x42, 0x37 } };
const GUID kIID_ILatLongReport = { 0x7fed806d, 0x0ef8, 0x4f07, { 0x80, 0xac, 0x36, 0xa0, 0xbe, 0xae, 0x31, 0x34 } };

struct ThemeSettings {
    bool enabled    = false;
    bool bySchedule = false;    // false = за сонцем
    int  darkFrom   = 19 * 60;  // хвилини від півночі
    int  lightFrom  = 7 * 60;
    LocSource src   = LocSource::Auto;
    bool   hasManual = false;   // ручні координати задано
    double lat = 0, lon = 0;    // ручні координати
};
struct ThemeFix {               // розташування, за яким рахуємо сонце
    bool   ok = false;
    double lat = 0, lon = 0;
    LocSource src = LocSource::Auto;
    __time64_t at = 0;
};
struct LocResult {              // відповідь потоку геолокації
    bool   ok = false;
    double lat = 0, lon = 0;
    LocSource src = LocSource::Auto;
    bool   prompt = false;      // показати системний діалог дозволу (лише явний вибір «Windows»)
    LONG   gen = 0;
};
ThemeSettings g_th;
ThemeFix      g_fix;
__time64_t    g_thOvUntil = 0;     // 0 = ручного вибору немає
bool          g_thOvDark  = false;
bool          g_thPending = false; // треба перемкнути, чекаємо закриття повноекранної програми
bool          g_locFailed = false; // остання спроба визначити розташування провалилась
bool          g_locPrompted = false;
bool          g_locAgain  = false; // джерело змінили під час визначення — повторити
volatile LONG g_locBusy = 0;
LONG          g_locGen  = 0;
HWND g_pageTheme[40] = {};  int g_pageThemeN = 0;
HWND g_thAdv[20]     = {};  int g_thAdvN = 0;
bool g_thAdvVisible = false;
HWND g_thEnable = nullptr, g_thBySun = nullptr, g_thBySched = nullptr;
HWND g_thDarkFrom = nullptr, g_thLightFrom = nullptr, g_thToggle = nullptr;
HWND g_thAdvBtn = nullptr, g_thStatus = nullptr, g_thNow = nullptr;
HWND g_thLat = nullptr, g_thLon = nullptr, g_thSrc[5] = {};

// ---------- CAPS-2: збільшення курсора по трусінню мишею ----------
//
// Збільшуємо САМ системний курсор (SystemParametersInfo 0x2029 -> CursorBaseSize),
// а не малюємо копію в оверлеї: апаратний курсор Windows малюється поверх усіх
// вікон, тож оверлейна копія завжди йшла б у парі з живим маленьким курсором.
// Ціна рішення — це глобальна настройка користувача, тому її треба вміти
// повернути навіть після аварійного завершення (kRegCursorRestore нижче).
constexpr UINT SPI_SETCURSORSIZE_ = 0x2029;  // недокументований, але стабільний з Win10
constexpr int  kCursorMinPx = 32;
constexpr int  kCursorMaxPx = 256;

const wchar_t* kRegCursorEnable    = L"CursorFind";
const wchar_t* kRegCursorScale     = L"CursorScale";
const wchar_t* kRegCursorHold      = L"CursorHoldMs";
const wchar_t* kRegCursorShrink    = L"CursorShrinkMs";
const wchar_t* kRegShakeWindow     = L"ShakeWindowMs";
const wchar_t* kRegShakeDistance   = L"ShakeMinDistance";
const wchar_t* kRegShakeFactor     = L"ShakeFactor";
const wchar_t* kRegShakeReversals  = L"ShakeReversals";
const wchar_t* kRegCursorOverlay   = L"CursorOverlayShrink";
const wchar_t* kRegCursorRestore   = L"CursorRestorePx";  // аварійний слід

// Дефолти підібрані на симуляції жестів (див. коментар біля ShakeFeed):
// 1000 px — найменший поріг, за якого жоден із перевірених «звичайних» рухів
// не проходить, а справжнє трусіння лишається коротким (4–6 махів, ~0.4 с).
struct CursorSettings {
    bool enabled   = true;
    int  scale     = 5;     // у скільки разів збільшувати (2..8)
    int  holdMs    = 1500;  // тримати збільшеним після жесту
    int  shrinkMs  = 250;   // тривалість плавного зменшення
    int  windowMs  = 700;   // вікно, у якому рахуємо рухи
    int  distance  = 1000;  // мінімальний пройдений шлях, px
    int  factor    = 350;   // шлях / діагональ габариту, %
    int  reversals = 3;     // мінімум змін напрямку
    bool overlay   = true;  // зменшувати намальованою копією, а не системним розміром
};
CursorSettings g_cur;

// Стан збільшення (живе на UI-потоці)
enum class MagState { Idle, Big, Shrinking };
MagState g_magState   = MagState::Idle;
int      g_magOrigPx  = kCursorMinPx;
int      g_magTargetPx = kCursorMinPx;

// Зменшення крутить ОКРЕМИЙ потік і рахує розмір від ЧАСУ, а не від номера кроку.
// Причина: кожне застосування розміру з SPIF_SENDCHANGE — синхронний бродкаст
// WM_SETTINGCHANGE усім вікнам, і його вартість залежить від того, скільки вікон
// відкрито й чи швидко вони відповідають (виміряно: 0.1 мс без бродкасту проти
// ~35 мс з ним на порожньому столі, і значно більше під навантаженням). Прив'язка
// до часу робить тривалість передбачуваною: на швидкій системі кроків більше й
// анімація гладка, на повільній — менше, але вкладаємось у ту саму чверть секунди.
CRITICAL_SECTION g_magLock;
volatile LONG    g_magGen = 0;     // покоління анімації; зміна = скасування
HANDLE           g_magThread = nullptr;

// Оверлейне зменшення. Системний розмір курсора анімувати неможливо: кожен кадр
// коштує синхронного бродкасту, і на завантаженій машині виходить 2-3 стрибки
// замість плавності. Тому тут малюємо ЗМЕНШУВАНУ КОПІЮ курсора у власному
// layered-вікні (це звичайна композиція GPU, десятки кадрів безкоштовно), а
// системний розмір повертаємо одним викликом у фоні. Плата — під копією видно
// справжній курсор; він уже нормального розміру й стоїть у тій самій точці.
HWND  g_overlay     = nullptr;
HICON g_overlayIcon = nullptr;
POINT g_ovHotspot   = {};
int   g_ovBasePx    = 32;   // розмір, у якому задано гарячу точку
int   g_ovFrom = 0, g_ovTo = 0;
DWORD g_ovStart = 0;
void  OverlayDestroy();   // визначення нижче, але потрібне вже у MagnifyRestore

// Буфер жесту (пишеться в колбеку хука, читається там само)
struct ShakeMove { int dx, dy; DWORD tick; };
constexpr int kMaxMoves = 64;
ShakeMove g_moves[kMaxMoves];
int   g_moveCount = 0;
POINT g_lastPt = {};
bool  g_haveLastPt = false;
DWORD g_shakeBlockUntil = 0;
unsigned g_btnMask = 0;   // які кнопки миші затиснуті зараз

// Хук живе на власному потоці (див. коментар біля HookThreadProc)
HANDLE g_hookThread = nullptr;
DWORD  g_hookThreadId = 0;
HWND   g_hookWnd = nullptr;   // message-only вікно того потоку для команд install/uninstall

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
    // Запитуємо реальну кількість, а не сподіваємось на фіксований розмір:
    // інакше в людини з багатьма розкладками поточна могла б не потрапити у
    // зрізаний список і перемикання стрибало б на першу.
    UINT n = GetKeyboardLayoutList(0, nullptr);
    if (n < 2) return nullptr;

    HKL list[64];
    if (n > 64) n = 64;
    n = GetKeyboardLayoutList(n, list);
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

// ---------- CAPS-1: виявлення remote/VM-вікон ----------
//
// Вікна цих процесів вважаємо клієнтом віддаленої/віртуальної машини. У них
// Caps треба ПРОПУСТИТИ, щоб розкладку перемкнула гостьова ОС (де теж стоїть
// capslang), а не перехоплювати його на хості. Список фіксований (v1).
bool IsRemoteWindow(HWND w)
{
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != FALSE;
    CloseHandle(h);
    if (!ok) return false;

    const wchar_t* name = PathFindFileNameW(path);
    static const wchar_t* const kRemoteProcs[] = {
        L"mstsc.exe",     // Remote Desktop (класичний RDP)
        L"msrdc.exe",     // Windows App / новий Remote Desktop-клієнт
        L"vmware.exe",    // VMware Workstation/Player (вікно консолі ВМ)
        L"vmconnect.exe", // Hyper-V (консоль підключення до ВМ)
    };
    for (const wchar_t* p : kRemoteProcs)
        if (lstrcmpiW(name, p) == 0)
            return true;
    return false;
}

bool RemotePassthroughActive() { return g_passthrough && g_inRemote; }

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

    // CAPS-1: у вікні віддаленої/віртуальної машини не перехоплюємо — Caps іде
    // далі, розкладку перемикає гостьова ОС.
    if (g_passthrough && g_inRemote) {
        g_capsDown = false;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

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

// ---------- CAPS-2: розпізнавання жесту ----------
//
// Дивимось не на швидкість, а на відношення пройденого шляху до діагоналі
// габаритного прямокутника руху. Прямий кидок через увесь екран дає відношення
// близько одиниці, трусіння — у рази більше. Саме це відсікає хибні спрацювання
// при звичайному швидкому наведенні, на яких свого часу погоріли перші версії
// подібної фічі в PowerToys.
int Sign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Виконується в колбеку хука, тому — тільки арифметика, жодних викликів,
// здатних заблокуватися: інакше LowLevelHooksTimeout і Windows зніме хук.
void ShakeFeed(POINT pt, DWORD now)
{
    if (!g_haveLastPt) { g_lastPt = pt; g_haveLastPt = true; return; }

    const int dx = pt.x - g_lastPt.x;
    const int dy = pt.y - g_lastPt.y;
    g_lastPt = pt;
    if (dx == 0 && dy == 0) return;

    // забути рухи, старші за вікно детекції
    int drop = 0;
    while (drop < g_moveCount && now - g_moves[drop].tick > (DWORD)g_cur.windowMs) drop++;
    if (drop > 0) {
        for (int i = drop; i < g_moveCount; ++i) g_moves[i - drop] = g_moves[i];
        g_moveCount -= drop;
    }

    const bool sameDir = g_moveCount > 0 &&
                         Sign(g_moves[g_moveCount - 1].dx) == Sign(dx) &&
                         Sign(g_moves[g_moveCount - 1].dy) == Sign(dy);
    if (sameDir) {
        g_moves[g_moveCount - 1].dx += dx;
        g_moves[g_moveCount - 1].dy += dy;
        g_moves[g_moveCount - 1].tick = now;
    } else {
        if (g_moveCount == kMaxMoves) {
            for (int i = 1; i < kMaxMoves; ++i) g_moves[i - 1] = g_moves[i];
            g_moveCount--;
        }
        g_moves[g_moveCount].dx = dx;
        g_moves[g_moveCount].dy = dy;
        g_moves[g_moveCount].tick = now;
        g_moveCount++;
    }

    if (g_moveCount - 1 < g_cur.reversals) return;
    if (now < g_shakeBlockUntil) return;

    double dist = 0.0;
    int x = 0, y = 0, minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (int i = 0; i < g_moveCount; ++i) {
        const ShakeMove& m = g_moves[i];
        dist += sqrt((double)m.dx * m.dx + (double)m.dy * m.dy);
        x += m.dx; y += m.dy;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    if (dist < g_cur.distance) return;

    const double bw = maxX - minX, bh = maxY - minY;
    double diag = sqrt(bw * bw + bh * bh);
    if (diag < 1.0) diag = 1.0;
    if (dist * 100.0 < (double)g_cur.factor * diag) return;

    // Жест зарахований. Буфер чистимо, щоб той самий розмах не тригерив двічі,
    // плюс короткий блок — інакше доведення руху одразу дає повторне спрацювання.
    g_moveCount = 0;
    g_shakeBlockUntil = now + 400;
    PostMessageW(g_mainWnd, WMAPP_SHAKE, 0, 0);
}

// Рухи із затиснутою кнопкою ігноруємо повністю. Це знімає найнеприємніший клас
// хибних спрацювань: ривкова перемотка повзунка, малювання/стирання в редакторі,
// перетягування вікна — на симуляції саме вони пролазили крізь усі пороги, бо
// формально це і є трусіння. Без кнопки таких рухів у житті не буває.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        switch (wParam) {
        case WM_LBUTTONDOWN: g_btnMask |= 1; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_RBUTTONDOWN: g_btnMask |= 2; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_MBUTTONDOWN: g_btnMask |= 4; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_XBUTTONDOWN: g_btnMask |= 8; g_moveCount = 0; g_haveLastPt = false; break;
        case WM_LBUTTONUP:   g_btnMask &= ~1u; g_haveLastPt = false; break;
        case WM_RBUTTONUP:   g_btnMask &= ~2u; g_haveLastPt = false; break;
        case WM_MBUTTONUP:   g_btnMask &= ~4u; g_haveLastPt = false; break;
        case WM_XBUTTONUP:   g_btnMask &= ~8u; g_haveLastPt = false; break;
        case WM_MOUSEMOVE:
            if (!g_btnMask) {
                const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
                ShakeFeed(m->pt, GetTickCount());
            }
            break;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// Потік хука: нічого не робить, крім циклу повідомлень, тож колбек
// обслуговується миттєво незалежно від того, чим зайнятий UI-потік. Команди
// install/uninstall приходять синхронно через SendMessage до цього вікна —
// SetWindowsHookEx мусить викликатися саме на тому потоці, де крутиться цикл,
// бо колбек виконується в його контексті.
LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case HKW_INSTALL:
        if (!g_hook)
            g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                       GetModuleHandleW(nullptr), 0);
        return g_hook != nullptr;
    case HKW_UNINSTALL:
        if (g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
        }
        return 0;
    case HKW_MOUSE_ON:
        if (!g_mouseHook) {
            g_haveLastPt = false;
            g_moveCount  = 0;
            g_btnMask    = 0;
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                            GetModuleHandleW(nullptr), 0);
        }
        return g_mouseHook != nullptr;
    case HKW_MOUSE_OFF:
        if (g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI HookThreadProc(LPVOID param)
{
    HANDLE ready = (HANDLE)param;
    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = HookWndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"capslang_hook";
    RegisterClassW(&wc);

    g_hookWnd = CreateWindowExW(0, L"capslang_hook", nullptr, 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    SetEvent(ready);  // головний потік чекає, поки вікно готове

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    return 0;
}

bool StartHookThread()
{
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, ready, 0, &g_hookThreadId);
    if (g_hookThread)
        WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    return g_hookThread != nullptr && g_hookWnd != nullptr;
}

void StopHookThread()
{
    if (g_hookThreadId)
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) {
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
}

// ---------- режим роботи ----------

// Реєстрація/зняття системного хоткея під бажаний стан.
// false — лише при реальній невдачі RegisterHotKey.
bool SetHotkey(bool want)
{
    if (want && !g_hotkeyActive) {
        if (!RegisterHotKey(g_mainWnd, HOTKEY_ID, MOD_NOREPEAT, VK_CAPITAL))
            return false;
        g_hotkeyActive = true;
    } else if (!want && g_hotkeyActive) {
        UnregisterHotKey(g_mainWnd, HOTKEY_ID);
        g_hotkeyActive = false;
    }
    return true;
}

void StopInterception()
{
    if (g_hookWnd)
        SendMessageW(g_hookWnd, HKW_UNINSTALL, 0, 0);  // на потоці хука
    SetHotkey(false);
    g_interceptionOn = false;
    g_capsDown = false;
}

bool StartInterception(Mode mode)
{
    if (mode == Mode::Hook) {
        bool ok = g_hookWnd && SendMessageW(g_hookWnd, HKW_INSTALL, 0, 0) != 0;
        if (ok) g_interceptionOn = true;
        return ok;
    }
    // Hotkey: якщо ми зараз у remote-вікні з увімкненим пропуском — свідомо НЕ
    // реєструємо (щоб Caps ішов у клієнта); зареєструємо при виході з нього.
    if (RemotePassthroughActive()) {
        g_hotkeyActive = false;
        g_interceptionOn = true;
        return true;
    }
    if (!SetHotkey(true))
        return false;
    g_interceptionOn = true;
    return true;
}

// CAPS-1: привести перехоплення до поточного контексту (режим/налаштування/вікно).
// Hook: колбек читає прапорці наживо. Hotkey: тримаємо реєстрацію лише поза
// remote-вікнами (або коли пропуск вимкнено).
void ApplyRemoteContext()
{
    if (g_interceptionOn && g_mode == Mode::Hotkey)
        SetHotkey(!RemotePassthroughActive());
}

void ThemeTick();   // CAPS-7, визначення нижче

// Зміна активного вікна: оновлюємо ознаку remote і підлаштовуємо перехоплення.
// Викликається з WinEvent-колбека на головному потоці — тому RegisterHotKey
// коректно виконується на потоці-власнику g_mainWnd.
void OnForegroundChanged()
{
    g_inRemote = IsRemoteWindow(GetForegroundWindow());
    ApplyRemoteContext();
    if (g_thPending) ThemeTick();   // CAPS-7: повноекранна програма могла закритись
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD)
{
    if (event == EVENT_SYSTEM_FOREGROUND)
        OnForegroundChanged();
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

bool LoadPassthrough()
{
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, kRegPassthrough, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return true;  // за замовчуванням увімкнено
}

void SavePassthrough(bool on)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD value = on ? 1 : 0;
    RegSetValueExW(key, kRegPassthrough, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(key);
}

// ---------- CAPS-2: налаштування курсора в реєстрі ----------

int RegLoadInt(const wchar_t* name, int def, int lo, int hi)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS)
        return def;
    int v = (int)value;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void RegSaveInt(const wchar_t* name, int value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = (DWORD)value;
    RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
}

void RegDeleteInt(const wchar_t* name)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
}

void LoadCursorSettings()
{
    g_cur.enabled   = RegLoadInt(kRegCursorEnable,   1,    0,    1) != 0;
    g_cur.scale     = RegLoadInt(kRegCursorScale,    5,    2,    8);
    g_cur.holdMs    = RegLoadInt(kRegCursorHold,     1500, 500,  5000);
    g_cur.shrinkMs  = RegLoadInt(kRegCursorShrink,   300,  100,  1500);
    g_cur.windowMs  = RegLoadInt(kRegShakeWindow,    700,  300,  2000);
    g_cur.distance  = RegLoadInt(kRegShakeDistance,  1200, 300,  5000);
    g_cur.factor    = RegLoadInt(kRegShakeFactor,    350,  150,  1000);
    g_cur.reversals = RegLoadInt(kRegShakeReversals, 3,    2,    10);
    g_cur.overlay   = RegLoadInt(kRegCursorOverlay,  1,    0,    1) != 0;
}

// ---------- CAPS-2: власне збільшення ----------

// Поточний розмір курсора користувача в пікселях (шкала Windows 32..256).
int CursorSizePx()
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        value >= (DWORD)kCursorMinPx && value <= (DWORD)kCursorMaxPx)
        return (int)value;
    return kCursorMinPx;
}

// SPIF_UPDATEINIFILE тут свідомо: без запису в профіль частина складань Windows
// не застосовує розмір одразу, а ми все одно зобовʼязані вміти повернути
// вихідне значення (див. kRegCursorRestore), тож персистентність нічого не псує.
void ApplyCursorSizePx(int px)
{
    if (px < kCursorMinPx) px = kCursorMinPx;
    if (px > kCursorMaxPx) px = kCursorMaxPx;
    SystemParametersInfoW(SPI_SETCURSORSIZE_, 0, (PVOID)(INT_PTR)px,
                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

// Повноекранні застосунки (вимога тікета): ігри не повинні ловити наш жест.
// SHQueryUserNotificationState ловить exclusive-D3D і презентаційний режим, але
// мовчить про borderless-вікна, тому додаємо геометричну перевірку — власник
// просив блокувати все, що ПОВОДИТЬСЯ як повноекранна гра.
bool IsFullscreenForeground()
{
    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE ||
         state == QUNS_BUSY))
        return true;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    // Робочий стіл і панель задач теж «на весь екран» і без рамки — це не гра.
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!lstrcmpiW(cls, L"Progman") || !lstrcmpiW(cls, L"WorkerW") ||
        !lstrcmpiW(cls, L"Shell_TrayWnd"))
        return false;

    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    if (!EqualRect(&wr, &mi.rcMonitor)) return false;   // не рівно на монітор

    // Звичайне максимізоване вікно має заголовок/рамку — його не чіпаємо.
    const LONG style = GetWindowLongW(fg, GWL_STYLE);
    return (style & (WS_CAPTION | WS_THICKFRAME)) == 0;
}

// Застосувати розмір, але лише якщо анімація ще актуальна. gen == 0 — виклик із
// UI-потоку, він завжди має пріоритет; лок не дає потоку анімації втиснути свій
// проміжний кадр уже після того, як UI вирішив інше.
void ApplyCursorSizeGuarded(int px, LONG gen)
{
    EnterCriticalSection(&g_magLock);
    if (gen == 0 || g_magGen == gen)
        ApplyCursorSizePx(px);
    LeaveCriticalSection(&g_magLock);
}

void CancelMagAnimation()
{
    InterlockedIncrement(&g_magGen);
    if (g_magThread) {
        WaitForSingleObject(g_magThread, 500);
        CloseHandle(g_magThread);
        g_magThread = nullptr;
    }
}

void MagnifyRestore()
{
    KillTimer(g_mainWnd, TIMER_MAG_HOLD);
    KillTimer(g_mainWnd, TIMER_MAG_FRAME);
    OverlayDestroy();
    CancelMagAnimation();
    if (g_magState != MagState::Idle)
        ApplyCursorSizeGuarded(g_magOrigPx, 0);
    RegDeleteInt(kRegCursorRestore);
    g_magState = MagState::Idle;
}

void MagnifyStart()
{
    if (!g_cur.enabled || !g_mainWnd) return;
    if (IsFullscreenForeground()) return;

    if (g_magState == MagState::Idle) {
        g_magOrigPx = CursorSizePx();
        // слід на випадок аварійного завершення: наступний старт поверне розмір
        RegSaveInt(kRegCursorRestore, g_magOrigPx);
        int target = g_magOrigPx * g_cur.scale;
        if (target > kCursorMaxPx) target = kCursorMaxPx;
        g_magTargetPx = target;
        ApplyCursorSizeGuarded(target, 0);
    } else if (g_magState == MagState::Shrinking) {
        KillTimer(g_mainWnd, TIMER_MAG_FRAME);       // потрусили ще раз під час
        OverlayDestroy();                            // зменшення — вертаємо великий
        CancelMagAnimation();
        ApplyCursorSizeGuarded(g_magTargetPx, 0);
    }

    g_magState = MagState::Big;
    SetTimer(g_mainWnd, TIMER_MAG_HOLD, (UINT)g_cur.holdMs, nullptr);
}

// Плавне зменшення потрібне, щоб око встигло провести курсор до справжнього
// розміру (вимога тікета). Крива ease-out: спочатку швидко, під кінець м'яко.
DWORD WINAPI MagShrinkThread(LPVOID param)
{
    const LONG gen = (LONG)(LONG_PTR)param;
    const int from = g_magTargetPx, to = g_magOrigPx;
    const DWORD duration = (DWORD)g_cur.shrinkMs;
    const DWORD start = GetTickCount();

    int last = from;
    for (;;) {
        if (g_magGen != gen) return 0;               // скасовано новим жестом
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= duration) break;
        double t = (double)elapsed / duration;
        t = 1.0 - (1.0 - t) * (1.0 - t);
        // Windows має власну сходинку розмірів курсора (32 px + кратне 16), тож
        // проміжні значення між сходинками виглядають однаково, а коштують по
        // повному бродкасту. Округлюємо — удвічі менше викликів без втрати плавності.
        int px = (int)(from + (to - from) * t);
        px = kCursorMinPx + ((px - kCursorMinPx + 8) / 16) * 16;
        if (px != last) {
            ApplyCursorSizeGuarded(px, gen);
            last = px;
        }
        Sleep(8);
    }
    if (g_magGen == gen) {
        ApplyCursorSizeGuarded(to, gen);
        PostMessageW(g_mainWnd, WMAPP_MAGDONE, 0, (LPARAM)gen);
    }
    return 0;
}

// ---------- оверлейне зменшення ----------

void OverlayDestroy()
{
    if (g_overlay) { DestroyWindow(g_overlay); g_overlay = nullptr; }
    if (g_overlayIcon) { DestroyIcon(g_overlayIcon); g_overlayIcon = nullptr; }
}

// Малюємо копію курсора заданого розміру в layered-вікно під гарячою точкою.
void OverlayFrame(int size)
{
    if (!g_overlay || !g_overlayIcon || size < 1) return;

    POINT pt;
    GetCursorPos(&pt);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        DrawIconEx(mem, 0, 0, g_overlayIcon, size, size, 0, nullptr, DI_NORMAL);

        // UpdateLayeredWindow хоче premultiplied alpha. Курсори з 1-бітною маскою
        // приходять із нульовою альфою — тоді копія була б невидимою, тож такі
        // пікселі робимо непрозорими за наявністю кольору.
        BYTE* p = (BYTE*)bits;
        const int count = size * size;
        bool anyAlpha = false;
        for (int i = 0; i < count; ++i)
            if (p[i * 4 + 3]) { anyAlpha = true; break; }
        for (int i = 0; i < count; ++i) {
            BYTE* px = p + i * 4;
            if (!anyAlpha)
                px[3] = (px[0] || px[1] || px[2]) ? 255 : 0;
            const int a = px[3];
            px[0] = (BYTE)(px[0] * a / 255);
            px[1] = (BYTE)(px[1] * a / 255);
            px[2] = (BYTE)(px[2] * a / 255);
        }

        POINT dst = { pt.x - MulDiv(g_ovHotspot.x, size, g_ovBasePx),
                      pt.y - MulDiv(g_ovHotspot.y, size, g_ovBasePx) };
        SIZE  wnd = { size, size };
        POINT src = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_overlay, screen, &dst, &wnd, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// Системний розмір повертаємо у фоні: один виклик, але дорогий, і блокувати ним
// анімацію не можна.
DWORD WINAPI RestoreSizeThread(LPVOID param)
{
    const LONG gen = (LONG)(LONG_PTR)param;
    ApplyCursorSizeGuarded(g_magOrigPx, gen);
    // слід у реєстрі прибираємо аж тут: поки системний розмір не повернувся
    // насправді, аварійне завершення має лишати можливість його відновити
    PostMessageW(g_mainWnd, WMAPP_MAGDONE, 0, (LPARAM)gen);
    return 0;
}

bool OverlayBeginShrink()
{
    CURSORINFO ci = { sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor || !(ci.flags & CURSOR_SHOWING))
        return false;
    HICON copy = CopyIcon(ci.hCursor);
    if (!copy) return false;

    ICONINFO ii = {};
    if (GetIconInfo(copy, &ii)) {
        g_ovHotspot.x = (LONG)ii.xHotspot;
        g_ovHotspot.y = (LONG)ii.yHotspot;
        BITMAP bm = {};
        HBITMAP src = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
        g_ovBasePx = (GetObjectW(src, sizeof(bm), &bm) && bm.bmWidth > 0) ? bm.bmWidth : 32;
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    } else {
        g_ovHotspot.x = g_ovHotspot.y = 0;
        g_ovBasePx = 32;
    }

    OverlayDestroy();
    g_overlayIcon = copy;
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                L"capslang_overlay", nullptr, WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!g_overlay) { OverlayDestroy(); return false; }
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);

    g_ovFrom  = g_magTargetPx;
    g_ovTo    = g_magOrigPx;
    g_ovStart = GetTickCount();
    OverlayFrame(g_ovFrom);

    // системний розмір вертаємо паралельно — копія прикриє момент перемикання
    const LONG gen = InterlockedIncrement(&g_magGen);
    if (HANDLE t = CreateThread(nullptr, 0, RestoreSizeThread, (LPVOID)(LONG_PTR)gen, 0, nullptr))
        CloseHandle(t);

    SetTimer(g_mainWnd, TIMER_MAG_FRAME, 16, nullptr);   // ~60 кадрів/с
    return true;
}

void OverlayFrameTick()
{
    const DWORD elapsed = GetTickCount() - g_ovStart;
    const DWORD duration = (DWORD)g_cur.shrinkMs;
    if (elapsed >= duration) {
        KillTimer(g_mainWnd, TIMER_MAG_FRAME);
        OverlayDestroy();
        g_magState = MagState::Idle;   // слід у реєстрі знімає RestoreSizeThread
        return;
    }
    double t = (double)elapsed / duration;
    t = 1.0 - (1.0 - t) * (1.0 - t);            // ease-out
    OverlayFrame((int)(g_ovFrom + (g_ovTo - g_ovFrom) * t));
}

void MagnifyBeginShrink()
{
    KillTimer(g_mainWnd, TIMER_MAG_HOLD);
    if (g_magState != MagState::Big) return;
    CancelMagAnimation();
    g_magState = MagState::Shrinking;

    if (g_cur.overlay && OverlayBeginShrink())
        return;

    const LONG gen = InterlockedIncrement(&g_magGen);
    g_magThread = CreateThread(nullptr, 0, MagShrinkThread,
                               (LPVOID)(LONG_PTR)gen, 0, nullptr);
    if (!g_magThread)          // потік не створився — просто повертаємо розмір
        MagnifyRestore();
}

// Якщо попередній запуск помер із великим курсором — повертаємо розмір.
void RecoverCursorSize()
{
    const int px = RegLoadInt(kRegCursorRestore, 0, 0, kCursorMaxPx);
    if (px >= kCursorMinPx) {
        ApplyCursorSizePx(px);
        RegDeleteInt(kRegCursorRestore);
    }
}

// Мишачий хук тримаємо лише поки фіча ввімкнена — зайвий глобальний хук
// у системі не потрібен.
void ApplyCursorFeature()
{
    if (!g_hookWnd) return;
    if (g_cur.enabled) {
        SendMessageW(g_hookWnd, HKW_MOUSE_ON, 0, 0);
    } else {
        SendMessageW(g_hookWnd, HKW_MOUSE_OFF, 0, 0);
        MagnifyRestore();
    }
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

// ---------- CAPS-7: день/ніч — реєстр, час, сонце ----------

bool RegLoadStr(const wchar_t* name, wchar_t* buf, DWORD cch)
{
    DWORD size = cch * sizeof(wchar_t);
    return RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_SZ,
                        nullptr, buf, &size) == ERROR_SUCCESS;
}

void RegSaveStr(const wchar_t* name, const wchar_t* value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value,
                   (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// Координата з поля/реєстру: приймаємо і «50.77», і «50,77».
bool ParseCoord(const wchar_t* s, double lo, double hi, double& out)
{
    wchar_t tmp[32] = {};
    for (int i = 0; i < 31 && s[i]; ++i) tmp[i] = (s[i] == L',') ? L'.' : s[i];
    wchar_t* end = nullptr;
    const double v = wcstod(tmp, &end);
    if (end == tmp || v < lo || v > hi) return false;
    out = v;
    return true;
}

__time64_t NowUnix() { return _time64(nullptr); }

void LocalDate(__time64_t t, int& y, int& m, int& d, int& minOfDay)
{
    struct tm lt = {};
    _localtime64_s(&lt, &t);
    y = lt.tm_year + 1900; m = lt.tm_mon + 1; d = lt.tm_mday;
    minOfDay = lt.tm_hour * 60 + lt.tm_min;
}

void FormatClock(wchar_t* buf, size_t n, __time64_t t)
{
    struct tm lt = {};
    _localtime64_s(&lt, &t);
    swprintf(buf, n, L"%02d:%02d", lt.tm_hour, lt.tm_min);
}

// ---- CAPS-7: sun math begin ----
constexpr double kPi = 3.14159265358979323846;
double Rad(double d) { return d * kPi / 180.0; }
double Deg(double r) { return r * 180.0 / kPi; }

double JulianDay(int y, int m, int d)   // 0:00 UTC заданої дати
{
    if (m <= 2) { y--; m += 12; }
    const int A = y / 100, B = 2 - A + A / 4;
    return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + B - 1524.5;
}

// Схід/захід за NOAA (точність ~1 хв). riseMin/setMin — хвилини UTC від 0:00 дати
// (можуть виходити за межі доби для далеких поясів). false = сонце цієї доби не
// сходить (polarDay=false) або не заходить (polarDay=true).
bool SunTimesUtc(int y, int m, int d, double lat, double lon,
                 double& riseMin, double& setMin, bool& polarDay)
{
    const double jc = (JulianDay(y, m, d) + 0.5 - 2451545.0) / 36525.0;
    const double L0 = fmod(280.46646 + jc * (36000.76983 + jc * 0.0003032), 360.0);
    const double M  = 357.52911 + jc * (35999.05029 - 0.0001537 * jc);
    const double e  = 0.016708634 - jc * (0.000042037 + 0.0000001267 * jc);
    const double C  = sin(Rad(M)) * (1.914602 - jc * (0.004817 + 0.000014 * jc))
                    + sin(Rad(2 * M)) * (0.019993 - 0.000101 * jc)
                    + sin(Rad(3 * M)) * 0.000289;
    const double omega   = 125.04 - 1934.136 * jc;
    const double appLong = L0 + C - 0.00569 - 0.00478 * sin(Rad(omega));
    const double obl0 = 23.0 + (26.0 + (21.448 - jc * (46.815 + jc * (0.00059 - jc * 0.001813))) / 60.0) / 60.0;
    const double obl  = obl0 + 0.00256 * cos(Rad(omega));
    const double decl = asin(sin(Rad(obl)) * sin(Rad(appLong)));
    const double yy   = tan(Rad(obl / 2)) * tan(Rad(obl / 2));
    const double eqTime = 4 * Deg(yy * sin(2 * Rad(L0)) - 2 * e * sin(Rad(M))
                          + 4 * e * yy * sin(Rad(M)) * cos(2 * Rad(L0))
                          - 0.5 * yy * yy * sin(4 * Rad(L0)) - 1.25 * e * e * sin(2 * Rad(M)));
    const double cosHa = cos(Rad(90.833)) / (cos(Rad(lat)) * cos(decl)) - tan(Rad(lat)) * tan(decl);
    if (cosHa >= 1.0)  { polarDay = false; return false; }
    if (cosHa <= -1.0) { polarDay = true;  return false; }
    const double ha   = Deg(acos(cosHa));
    const double noon = 720.0 - 4.0 * lon - eqTime;
    riseMin = noon - ha * 4.0;
    setMin  = noon + ha * 4.0;
    return true;
}

// Схід/захід (unix) для локальної дати, що містить t. 0 = ок, 1 = полярна ніч,
// 2 = полярний день.
int SunEventsFor(__time64_t t, double lat, double lon, __time64_t& rise, __time64_t& set)
{
    int y, m, d, mod;
    LocalDate(t, y, m, d, mod);
    double r = 0, s = 0; bool pd = false;
    if (!SunTimesUtc(y, m, d, lat, lon, r, s, pd)) return pd ? 2 : 1;
    struct tm g = {};
    g.tm_year = y - 1900; g.tm_mon = m - 1; g.tm_mday = d;
    const __time64_t base = _mkgmtime64(&g);
    rise = base + (__time64_t)llround(r * 60.0);
    set  = base + (__time64_t)llround(s * 60.0);
    return 0;
}
// ---- CAPS-7: sun math end ----

// Що має бути зараз за налаштуваннями (без урахування ручного вибору) і коли
// наступна межа. usedFallback — координат нема, тимчасово рахуємо за 07:00/19:00.
bool ThemeWantDark(__time64_t now, __time64_t& nextBoundary, bool& usedFallback)
{
    usedFallback = false;
    if (!g_th.bySchedule && g_fix.ok) {
        __time64_t rise = 0, set = 0;
        const int kind = SunEventsFor(now, g_fix.lat, g_fix.lon, rise, set);
        if (kind == 0) {
            if (now < rise) { nextBoundary = rise; return true; }
            if (now < set)  { nextBoundary = set;  return false; }
            __time64_t r2 = 0, s2 = 0;      // після заходу — до завтрашнього сходу
            nextBoundary = (SunEventsFor(now + 86400, g_fix.lat, g_fix.lon, r2, s2) == 0)
                           ? r2 : now + 86400;
            return true;
        }
        nextBoundary = now + 6 * 3600;   // полярний день/ніч — перевіримо пізніше
        return kind == 1;
    }
    int df = g_th.darkFrom, lf = g_th.lightFrom;
    if (!g_th.bySchedule) { usedFallback = true; df = 19 * 60; lf = 7 * 60; }
    int y, m, d, mod;
    LocalDate(now, y, m, d, mod);
    auto at = [&](int minutes, int dayOffset) {
        struct tm lt = {};
        lt.tm_year = y - 1900; lt.tm_mon = m - 1; lt.tm_mday = d + dayOffset;
        lt.tm_hour = minutes / 60; lt.tm_min = minutes % 60; lt.tm_isdst = -1;
        return _mktime64(&lt);    // нормалізує d+1 і DST сама
    };
    if (df == lf) { nextBoundary = at(df, mod < df ? 0 : 1); return false; }
    const bool dark = (df < lf) ? (mod >= df && mod < lf) : (mod >= df || mod < lf);
    const __time64_t cands[4] = { at(df, 0), at(lf, 0), at(df, 1), at(lf, 1) };
    nextBoundary = 0;
    for (const __time64_t c : cands)
        if (c > now && (nextBoundary == 0 || c < nextBoundary)) nextBoundary = c;
    return dark;
}

bool ThemeIsDark()
{
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

// Бродкаст — на окремому потоці: SendMessageTimeout чекає на кожне вікно, і
// зависле вікно не має морозити наш UI.
DWORD WINAPI ThemeBroadcastThread(LPVOID)
{
    DWORD_PTR res = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                        SMTO_ABORTIFHUNG, 2000, &res);
    return 0;
}

void ThemeApply(bool dark)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPersonalize, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const DWORD v = dark ? 0 : 1;
    RegSetValueExW(key, L"AppsUseLightTheme",   0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegSetValueExW(key, L"SystemUsesLightTheme", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
    if (HANDLE t = CreateThread(nullptr, 0, ThemeBroadcastThread, nullptr, 0, nullptr))
        CloseHandle(t);
}

// ---------- CAPS-7: розташування ----------

bool LocateWindows(double& lat, double& lon, bool allowPrompt)
{
    ILocation* loc = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_Location, nullptr, CLSCTX_INPROC_SERVER,
                                kIID_ILocation, (void**)&loc)) || !loc)
        return false;
    bool ok = false;
    IID types[1] = { kIID_ILatLongReport };
    if (allowPrompt) loc->RequestPermissions(nullptr, types, 1, TRUE);
    for (int i = 0; i < 20; ++i) {           // до ~10 с: сенсор може прокидатись
        LOCATION_REPORT_STATUS st = REPORT_NOT_SUPPORTED;
        if (FAILED(loc->GetReportStatus(kIID_ILatLongReport, &st))) break;
        if (st == REPORT_RUNNING) {
            ILocationReport* rep = nullptr;
            if (SUCCEEDED(loc->GetReport(kIID_ILatLongReport, &rep)) && rep) {
                ILatLongReport* ll = nullptr;
                if (SUCCEEDED(rep->QueryInterface(kIID_ILatLongReport, (void**)&ll)) && ll) {
                    double la = 0, lo = 0;
                    if (SUCCEEDED(ll->GetLatitude(&la)) && SUCCEEDED(ll->GetLongitude(&lo))) {
                        lat = la; lon = lo; ok = true;
                    }
                    ll->Release();
                }
                rep->Release();
            }
            break;
        }
        if (st == REPORT_ACCESS_DENIED || st == REPORT_NOT_SUPPORTED || st == REPORT_ERROR)
            break;
        Sleep(500);
    }
    loc->Release();
    return ok;
}

// Один GET до ip-api.com (без ключа, HTTP — координати міста, не секрет).
bool LocateIp(double& lat, double& lon)
{
    HINTERNET h = InternetOpenW(L"capslang", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!h) return false;
    DWORD to = 8000;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    bool ok = false;
    HINTERNET u = InternetOpenUrlW(h, L"http://ip-api.com/json/?fields=status,lat,lon", nullptr, 0,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (u) {
        char buf[1024] = {};
        DWORD n = 0, total = 0;
        while (total < sizeof(buf) - 1 &&
               InternetReadFile(u, buf + total, (DWORD)(sizeof(buf) - 1 - total), &n) && n > 0)
            total += n;
        buf[total] = 0;
        const char* pla = strstr(buf, "\"lat\":");
        const char* plo = strstr(buf, "\"lon\":");
        if (strstr(buf, "\"status\":\"success\"") && pla && plo) {
            lat = atof(pla + 6); lon = atof(plo + 6);
            ok = fabs(lat) <= 90 && fabs(lon) <= 180 && (lat != 0 || lon != 0);
        }
        InternetCloseHandle(u);
    }
    InternetCloseHandle(h);
    return ok;
}

// Найгрубіше: довгота з UTC-зміщення (60 хв = 15°), широта/довгота країни з
// регіону Windows, якщо він є. Похибка сходу/заходу — до години.
bool LocateTimeZone(double& lat, double& lon)
{
    TIME_ZONE_INFORMATION tzi = {};
    if (GetTimeZoneInformation(&tzi) == TIME_ZONE_ID_INVALID) return false;
    lon = -tzi.Bias / 4.0;
    lat = 50.0;
    wchar_t buf[32] = {};
    const GEOID g = GetUserGeoID(GEOCLASS_NATION);
    double v = 0;
    if (g != GEOID_NOT_AVAILABLE && GetGeoInfoW(g, GEO_LATITUDE, buf, 32, 0) > 0 && ParseCoord(buf, -90, 90, v))
        lat = v;
    if (g != GEOID_NOT_AVAILABLE && GetGeoInfoW(g, GEO_LONGITUDE, buf, 32, 0) > 0 && ParseCoord(buf, -180, 180, v))
        lon = v;
    return true;
}

DWORD WINAPI LocateThread(LPVOID p)
{
    LocResult* r = (LocResult*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    r->ok = false;
    auto tryWin = [&](bool prompt) { if (!r->ok && LocateWindows(r->lat, r->lon, prompt)) { r->src = LocSource::Windows;  r->ok = true; } };
    auto tryIp  = [&]()            { if (!r->ok && LocateIp(r->lat, r->lon))              { r->src = LocSource::Ip;       r->ok = true; } };
    auto tryTz  = [&]()            { if (!r->ok && LocateTimeZone(r->lat, r->lon))        { r->src = LocSource::TimeZone; r->ok = true; } };
    switch (r->src) {
    case LocSource::Windows:  tryWin(r->prompt); break;
    case LocSource::Ip:       tryIp();  break;
    case LocSource::TimeZone: tryTz();  break;
    default:                  tryWin(false); tryIp(); tryTz(); break;   // Auto: без діалогів
    }
    CoUninitialize();
    PostMessageW(g_mainWnd, WMAPP_THEMELOC, 0, (LPARAM)r);
    return 0;
}

void SaveFixCache()
{
    wchar_t b[32];
    swprintf(b, 32, L"%.5f", g_fix.lat); RegSaveStr(kRegThemeCacheLat, b);
    swprintf(b, 32, L"%.5f", g_fix.lon); RegSaveStr(kRegThemeCacheLon, b);
    RegSaveInt(kRegThemeCacheSrc, (int)g_fix.src);
    RegSaveInt(kRegThemeCacheAt,  (int)(DWORD)g_fix.at);
}

void UseManualFix()
{
    g_fix.ok = g_th.hasManual;
    g_fix.lat = g_th.lat; g_fix.lon = g_th.lon;
    g_fix.src = LocSource::Manual;
    g_fix.at  = NowUnix();
}

void StartLocate()
{
    if (g_th.src == LocSource::Manual) { UseManualFix(); return; }
    if (InterlockedCompareExchange(&g_locBusy, 1, 0) != 0) return;   // уже визначаємо
    LocResult* r = new LocResult;
    r->src = g_th.src;
    r->gen = ++g_locGen;
    r->prompt = (g_th.src == LocSource::Windows) && !g_locPrompted;
    if (r->prompt) g_locPrompted = true;
    HANDLE t = CreateThread(nullptr, 0, LocateThread, r, 0, nullptr);
    if (!t) { delete r; g_locBusy = 0; return; }
    CloseHandle(t);
}

// ---------- CAPS-7: логіка перемикання ----------

void UpdateThemeStatus();   // нижче, у розділі UI

void ThemeTick()
{
    if (!g_th.enabled) return;
    const __time64_t now = NowUnix();
    __time64_t next = 0; bool fb = false;
    bool want = ThemeWantDark(now, next, fb);
    if (g_thOvUntil) {
        if (now < g_thOvUntil) want = g_thOvDark;
        else { g_thOvUntil = 0; RegDeleteInt(kRegThemeOvUntil); RegDeleteInt(kRegThemeOvDark); }
    }
    if (want != ThemeIsDark()) {
        if (IsFullscreenForeground()) g_thPending = true;
        else { ThemeApply(want); g_thPending = false; }
    } else {
        g_thPending = false;
    }
    // координати старіші за добу — оновити у фоні (сенсор/IP; ручні не старіють)
    if (!g_th.bySchedule && g_th.src != LocSource::Manual && now - g_fix.at > 86400)
        StartLocate();
    UpdateThemeStatus();
}

void ThemeToggleNow()
{
    const bool target = !ThemeIsDark();
    ThemeApply(target);
    g_thPending = false;
    if (g_th.enabled) {
        __time64_t next = 0; bool fb = false;
        ThemeWantDark(NowUnix(), next, fb);
        g_thOvUntil = next; g_thOvDark = target;
        RegSaveInt(kRegThemeOvUntil, (int)(DWORD)next);
        RegSaveInt(kRegThemeOvDark, target ? 1 : 0);
    }
    UpdateThemeStatus();
}

void LoadThemeSettings()
{
    g_th.enabled    = RegLoadInt(kRegThemeAuto,  0, 0, 1) != 0;
    g_th.bySchedule = RegLoadInt(kRegThemeSched, 0, 0, 1) != 0;
    g_th.darkFrom   = RegLoadInt(kRegThemeDarkFrom,  19 * 60, 0, 1439);
    g_th.lightFrom  = RegLoadInt(kRegThemeLightFrom, 7 * 60,  0, 1439);
    g_th.src        = (LocSource)RegLoadInt(kRegThemeLocSrc, 0, 0, 4);
    wchar_t b[32] = {};
    g_th.hasManual = RegLoadStr(kRegThemeLat, b, 32) && ParseCoord(b, -90, 90, g_th.lat)
                  && RegLoadStr(kRegThemeLon, b, 32) && ParseCoord(b, -180, 180, g_th.lon);
    if (RegLoadStr(kRegThemeCacheLat, b, 32) && ParseCoord(b, -90, 90, g_fix.lat)
     && RegLoadStr(kRegThemeCacheLon, b, 32) && ParseCoord(b, -180, 180, g_fix.lon)) {
        g_fix.ok  = true;
        g_fix.src = (LocSource)RegLoadInt(kRegThemeCacheSrc, 0, 0, 4);
        g_fix.at  = (DWORD)RegLoadInt(kRegThemeCacheAt, 0, INT_MIN, INT_MAX);
    }
    if (g_th.src == LocSource::Manual) UseManualFix();
    g_thOvUntil = (DWORD)RegLoadInt(kRegThemeOvUntil, 0, INT_MIN, INT_MAX);
    g_thOvDark  = RegLoadInt(kRegThemeOvDark, 0, 0, 1) != 0;
}

void SaveThemeSettings()
{
    RegSaveInt(kRegThemeAuto,      g_th.enabled ? 1 : 0);
    RegSaveInt(kRegThemeSched,     g_th.bySchedule ? 1 : 0);
    RegSaveInt(kRegThemeDarkFrom,  g_th.darkFrom);
    RegSaveInt(kRegThemeLightFrom, g_th.lightFrom);
    RegSaveInt(kRegThemeLocSrc,    (int)g_th.src);
}

// Версія з VERSIONINFO самого exe — єдине джерело лишається capslang.rc.
void ExeVersionString(wchar_t* buf, size_t n)
{
    buf[0] = 0;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return;
    BYTE* data = new BYTE[size];
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\", (LPVOID*)&ffi, &len) && ffi)
        swprintf(buf, n, L"%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                 LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS));
    delete[] data;
}

// ---------- CAPS-2: вкладки ----------

void ShowGroup(HWND* items, int n, bool show)
{
    for (int i = 0; i < n; ++i)
        ShowWindow(items[i], show ? SW_SHOW : SW_HIDE);
}

// Контроли сторінок — діти головного вікна, тож за замовчуванням вони малюють
// підкладку кольором діалогу й на білому полотні вкладки виглядають як сірі
// плашки. Тому таким контролам віддаємо колір вікна, решті — колір діалогу.
bool IsPageControl(HWND c)
{
    for (int i = 0; i < g_pageLayoutN; ++i) if (g_pageLayout[i] == c) return true;
    for (int i = 0; i < g_pageCursorN; ++i) if (g_pageCursor[i] == c) return true;
    for (int i = 0; i < g_advN; ++i)        if (g_advCtrls[i]   == c) return true;
    for (int i = 0; i < g_pageThemeN; ++i)  if (g_pageTheme[i]  == c) return true;
    for (int i = 0; i < g_thAdvN; ++i)      if (g_thAdv[i]      == c) return true;
    return false;
}

void SetCursorValueLabels()
{
    wchar_t buf[64];
    wsprintfW(buf, L"%d×", g_cur.scale);
    SetWindowTextW(g_curScaleVal, buf);
    wsprintfW(buf, L"%d,%d с", g_cur.holdMs / 1000, (g_cur.holdMs % 1000) / 100);
    SetWindowTextW(g_curHoldVal, buf);
}

void SelectTab(int index)
{
    ShowGroup(g_pageLayout, g_pageLayoutN, index == 0);
    ShowGroup(g_pageCursor, g_pageCursorN, index == 1);
    ShowGroup(g_advCtrls, g_advN, index == 1 && g_advVisible);
    ShowGroup(g_pageTheme, g_pageThemeN, index == 2);
    ShowGroup(g_thAdv, g_thAdvN, index == 2 && g_thAdvVisible);
}

void ToggleAdvanced()
{
    g_advVisible = !g_advVisible;
    SetWindowTextW(g_curAdvBtn, g_advVisible ? L"Детально ▴" : L"Детально ▾");
    ShowGroup(g_advCtrls, g_advN, g_advVisible);
}

// ---------- CAPS-7: UI вкладки «День/ніч» ----------

void ToggleThemeAdvanced()
{
    g_thAdvVisible = !g_thAdvVisible;
    SetWindowTextW(g_thAdvBtn, g_thAdvVisible ? L"Детально ▴" : L"Детально ▾");
    ShowGroup(g_thAdv, g_thAdvN, g_thAdvVisible);
}

void SetPickerMinutes(HWND p, int minutes)
{
    SendMessageW(p, DTM_SETFORMATW, 0, (LPARAM)L"HH:mm");
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    st.wHour = (WORD)(minutes / 60); st.wMinute = (WORD)(minutes % 60);
    st.wSecond = 0; st.wMilliseconds = 0;
    SendMessageW(p, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
}

int GetPickerMinutes(HWND p, int fallback)
{
    SYSTEMTIME st = {};
    if (SendMessageW(p, DTM_GETSYSTEMTIME, 0, (LPARAM)&st) != GDT_VALID) return fallback;
    return st.wHour * 60 + st.wMinute;
}

const wchar_t* LocSourceName(LocSource s)
{
    switch (s) {
    case LocSource::Windows:  return L"служба Windows";
    case LocSource::Ip:       return L"за IP-адресою";
    case LocSource::Manual:   return L"задано вручну";
    case LocSource::TimeZone: return L"часовий пояс і регіон";
    default:                  return L"автоматично";
    }
}

void UpdateThemeStatus()
{
    wchar_t line[256] = {}, c1[8] = {}, c2[8] = {};
    const __time64_t now = NowUnix();
    if (g_th.bySchedule) {
        swprintf(line, 256, L"Розклад: темна тема з %02d:%02d, світла з %02d:%02d.",
                 g_th.darkFrom / 60, g_th.darkFrom % 60, g_th.lightFrom / 60, g_th.lightFrom % 60);
    } else if (g_fix.ok) {
        __time64_t r = 0, s = 0;
        const int k = SunEventsFor(now, g_fix.lat, g_fix.lon, r, s);
        wchar_t where[64];
        swprintf(where, 64, L"%.2f°%s %.2f°%s", fabs(g_fix.lat), g_fix.lat >= 0 ? L"N" : L"S",
                 fabs(g_fix.lon), g_fix.lon >= 0 ? L"E" : L"W");
        if (k == 0) {
            FormatClock(c1, 8, r); FormatClock(c2, 8, s);
            swprintf(line, 256, L"Схід %s · захід %s · %s · %s", c1, c2, where, LocSourceName(g_fix.src));
        } else {
            swprintf(line, 256, L"%s · %s · %s", k == 2 ? L"Полярний день" : L"Полярна ніч",
                     where, LocSourceName(g_fix.src));
        }
    } else if (g_locBusy) {
        lstrcpyW(line, L"Визначаю розташування…");
    } else if (g_th.src == LocSource::Manual) {
        lstrcpyW(line, L"Введіть широту й довготу в «Детально». Поки що — розклад 07:00/19:00.");
    } else {
        lstrcpyW(line, L"Розташування не визначено — тимчасово розклад 07:00/19:00. Джерело — у «Детально».");
    }
    SetWindowTextW(g_thStatus, line);

    const bool dark = ThemeIsDark();
    if (!g_th.enabled) {
        swprintf(line, 256, L"Зараз %s тема. Автоматика вимкнена.", dark ? L"темна" : L"світла");
        SetWindowTextW(g_thNow, line);
        return;
    }
    __time64_t next = 0; bool fb = false;
    ThemeWantDark(now, next, fb);
    wchar_t nb[8] = L"—";
    if (g_thOvUntil && now < g_thOvUntil) {
        FormatClock(nb, 8, g_thOvUntil);
        swprintf(line, 256, L"Зараз %s тема (обрано вручну) — автоматика повернеться о %s.",
                 dark ? L"темна" : L"світла", nb);
    } else if (g_thPending) {
        swprintf(line, 256, L"Перемкну на %s тему, щойно закриється повноекранна програма.",
                 dark ? L"світлу" : L"темну");
    } else {
        if (next) FormatClock(nb, 8, next);
        swprintf(line, 256, L"Зараз %s тема · наступне перемикання о %s.", dark ? L"темна" : L"світла", nb);
    }
    SetWindowTextW(g_thNow, line);
}

void EnableThemeControls()
{
    EnableWindow(g_thDarkFrom,  g_th.bySchedule);
    EnableWindow(g_thLightFrom, g_th.bySchedule);
    const bool manual = g_th.src == LocSource::Manual;
    EnableWindow(g_thLat, manual);
    EnableWindow(g_thLon, manual);
}

// Ручні координати приймаються, коли обидва поля валідні (широта ±90, довгота ±180).
void CommitManualCoords()
{
    wchar_t a[32] = {}, b[32] = {};
    GetWindowTextW(g_thLat, a, 31);
    GetWindowTextW(g_thLon, b, 31);
    double la = 0, lo = 0;
    if (ParseCoord(a, -90, 90, la) && ParseCoord(b, -180, 180, lo)) {
        g_th.lat = la; g_th.lon = lo; g_th.hasManual = true;
        swprintf(a, 32, L"%.4f", la); RegSaveStr(kRegThemeLat, a); SetWindowTextW(g_thLat, a);
        swprintf(b, 32, L"%.4f", lo); RegSaveStr(kRegThemeLon, b); SetWindowTextW(g_thLon, b);
        if (g_th.src == LocSource::Manual) { UseManualFix(); ThemeTick(); }
    }
    UpdateThemeStatus();
}

void ThemeApplySettings()
{
    SaveThemeSettings();
    EnableThemeControls();
    if (g_th.enabled) {
        SetTimer(g_mainWnd, TIMER_THEME, 60 * 1000, nullptr);
        if (!g_th.bySchedule &&
            (!g_fix.ok || (g_th.src != LocSource::Auto && g_fix.src != g_th.src)))
            StartLocate();
        ThemeTick();
    } else {
        KillTimer(g_mainWnd, TIMER_THEME);
        g_thPending = false;
        UpdateThemeStatus();
    }
}

// Прочитати число з поля «Детально», притиснути до допустимого діапазону і
// повернути в поле — щоб користувач бачив, що саме прийнято.
int ReadEditInt(HWND edit, int lo, int hi, int fallback)
{
    wchar_t buf[16] = {};
    GetWindowTextW(edit, buf, 15);
    int v = _wtoi(buf);
    if (v == 0 && buf[0] != L'0') v = fallback;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    wsprintfW(buf, L"%d", v);
    SetWindowTextW(edit, buf);
    return v;
}

void CommitAdvanced()
{
    g_cur.windowMs  = ReadEditInt(g_edWindow, 300, 2000, g_cur.windowMs);
    g_cur.distance  = ReadEditInt(g_edDist,   300, 5000, g_cur.distance);
    g_cur.factor    = ReadEditInt(g_edFactor, 150, 1000, g_cur.factor);
    g_cur.reversals = ReadEditInt(g_edRevers, 2,   10,   g_cur.reversals);
    g_cur.shrinkMs  = ReadEditInt(g_edShrink, 100, 1500, g_cur.shrinkMs);
    RegSaveInt(kRegShakeWindow,    g_cur.windowMs);
    RegSaveInt(kRegShakeDistance,  g_cur.distance);
    RegSaveInt(kRegShakeFactor,    g_cur.factor);
    RegSaveInt(kRegShakeReversals, g_cur.reversals);
    RegSaveInt(kRegCursorShrink,   g_cur.shrinkMs);
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

    case WMAPP_SHAKE:    // від мишачого хука
        MagnifyStart();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_MAG_HOLD)       MagnifyBeginShrink();
        else if (wp == TIMER_MAG_FRAME) OverlayFrameTick();
        else if (wp == TIMER_THEME)     ThemeTick();
        return 0;

    case WMAPP_THEMELOC: {   // CAPS-7: потік геолокації завершився
        LocResult* r = (LocResult*)lp;
        g_locBusy = 0;
        if (r->gen == g_locGen && g_th.src != LocSource::Manual) {
            if (r->ok) {
                g_fix.ok = true; g_fix.lat = r->lat; g_fix.lon = r->lon;
                g_fix.src = r->src; g_fix.at = NowUnix();
                g_locFailed = false;
                SaveFixCache();
            } else {
                g_locFailed = true;
                g_fix.at = NowUnix();   // не довбати сенсор/мережу щохвилини
            }
            ThemeTick();
            UpdateThemeStatus();
        } else if (g_locAgain) {
            g_locAgain = false;        // джерело змінили, поки тривало визначення
            StartLocate();
        }
        delete r;
        return 0;
    }

    case WM_POWERBROADCAST:   // CAPS-7: після сну тема має відповідати часу
        if (wp == PBT_APMRESUMEAUTOMATIC) ThemeTick();
        return TRUE;

    case WM_TIMECHANGE:       // CAPS-7: змінили час/пояс
        ThemeTick();
        return 0;

    case WMAPP_MAGDONE:   // системний розмір повернуто (lp = покоління анімації)
        if (g_magState == MagState::Shrinking && g_magGen == (LONG)lp) {
            RegDeleteInt(kRegCursorRestore);
            if (!g_overlay)          // при оверлеї стан закриє його ж таймер
                g_magState = MagState::Idle;
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)lp == g_curScale) {
            g_cur.scale = (int)SendMessageW(g_curScale, TBM_GETPOS, 0, 0);
            RegSaveInt(kRegCursorScale, g_cur.scale);
            SetCursorValueLabels();
        } else if ((HWND)lp == g_curHold) {
            g_cur.holdMs = (int)SendMessageW(g_curHold, TBM_GETPOS, 0, 0) * 100;
            RegSaveInt(kRegCursorHold, g_cur.holdMs);
            SetCursorValueLabels();
        }
        return 0;

    case WM_NOTIFY: {
        const NMHDR* nm = (const NMHDR*)lp;
        if (nm->hwndFrom == g_tabs && nm->code == TCN_SELCHANGE)
            SelectTab((int)SendMessageW(g_tabs, TCM_GETCURSEL, 0, 0));
        // CAPS-7: розклад дня/ночі
        if (nm->code == DTN_DATETIMECHANGE &&
            (nm->idFrom == IDC_TH_DARK_FROM || nm->idFrom == IDC_TH_LIGHT_FROM)) {
            g_th.darkFrom  = GetPickerMinutes(g_thDarkFrom,  g_th.darkFrom);
            g_th.lightFrom = GetPickerMinutes(g_thLightFrom, g_th.lightFrom);
            SaveThemeSettings();
            ThemeTick();
            UpdateThemeStatus();
        }
        // CAPS-7: посилання на GitHub у підвалі. Через explorer, бо capslang
        // елевейтований, а браузер має відкритись звичайним користувачем.
        if (nm->idFrom == IDC_COPYRIGHT && (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
            const NMLINK* l = (const NMLINK*)lp;
            ShellExecuteW(nullptr, L"open", L"explorer.exe", l->item.szUrl, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

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
                        L"Не вдалося змінити задачу автозапуску.",
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
        case IDC_PASSTHROUGH:
            if (HIWORD(wp) == BN_CLICKED) {
                g_passthrough = SendMessageW(g_passthroughCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SavePassthrough(g_passthrough);
                ApplyRemoteContext();
            }
            return 0;
        case IDC_CUR_ENABLE:
            if (HIWORD(wp) == BN_CLICKED) {
                g_cur.enabled = SendMessageW(g_curEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RegSaveInt(kRegCursorEnable, g_cur.enabled ? 1 : 0);
                ApplyCursorFeature();
            }
            return 0;
        case IDC_CUR_OVERLAY:
            if (HIWORD(wp) == BN_CLICKED) {
                g_cur.overlay = SendMessageW(g_curOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED;
                RegSaveInt(kRegCursorOverlay, g_cur.overlay ? 1 : 0);
            }
            return 0;
        case IDC_CUR_ADVANCED:
            if (HIWORD(wp) == BN_CLICKED)
                ToggleAdvanced();
            return 0;
        case IDC_CUR_WINDOWMS:
        case IDC_CUR_DIST:
        case IDC_CUR_FACTOR:
        case IDC_CUR_REVERSALS:
        case IDC_CUR_SHRINK:
            if (HIWORD(wp) == EN_KILLFOCUS)
                CommitAdvanced();
            return 0;
        // ---- CAPS-7: день/ніч ----
        case IDC_TH_ENABLE:
            if (HIWORD(wp) == BN_CLICKED) {
                g_th.enabled = SendMessageW(g_thEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
                ThemeApplySettings();
            }
            return 0;
        case IDC_TH_BY_SUN:
        case IDC_TH_BY_SCHED:
            if (HIWORD(wp) == BN_CLICKED) {
                g_th.bySchedule = (LOWORD(wp) == IDC_TH_BY_SCHED);
                ThemeApplySettings();
            }
            return 0;
        case IDC_TH_TOGGLE:
            if (HIWORD(wp) == BN_CLICKED) ThemeToggleNow();
            return 0;
        case IDC_TH_ADVANCED:
            if (HIWORD(wp) == BN_CLICKED) ToggleThemeAdvanced();
            return 0;
        case IDC_TH_SRC_AUTO:
        case IDC_TH_SRC_WIN:
        case IDC_TH_SRC_IP:
        case IDC_TH_SRC_MANUAL:
        case IDC_TH_SRC_TZ:
            if (HIWORD(wp) == BN_CLICKED) {
                g_th.src = (LocSource)(LOWORD(wp) - IDC_TH_SRC_AUTO);
                RegSaveInt(kRegThemeLocSrc, (int)g_th.src);
                EnableThemeControls();
                g_locFailed = false;
                if (g_th.src == LocSource::Manual) {
                    CommitManualCoords();          // сам зробить UseManualFix + ThemeTick
                } else if (g_locBusy) {
                    ++g_locGen;                    // відповідь, що летить, уже неактуальна
                    g_locAgain = true;
                } else {
                    StartLocate();
                }
                ThemeTick();
                UpdateThemeStatus();
            }
            return 0;
        case IDC_TH_LAT:
        case IDC_TH_LON:
            if (HIWORD(wp) == EN_KILLFOCUS) CommitManualCoords();
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

    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID((HWND)lp);
        const int color = IsPageControl((HWND)lp) ? COLOR_WINDOW : COLOR_BTNFACE;
        SetBkMode((HDC)wp, TRANSPARENT);
        SetBkColor((HDC)wp, GetSysColor(color));
        if (id == IDC_COPYRIGHT || id == IDC_PASSTHROUGH_HINT || id == IDC_HINT_GRAY)
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(color);
    }

    case WM_CLOSE:
        CommitAdvanced();          // підхопити те, що набрали й не зняли фокус
        CommitManualCoords();      // CAPS-7: те саме для координат
        ShowWindow(hwnd, SW_HIDE); // закриття вікна не завершує програму
        return 0;

    case WM_ENDSESSION:
        if (wp) MagnifyRestore();  // логаут/вимкнення — не лишати великий курсор
        return 0;

    case WM_DESTROY:
        MagnifyRestore();
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

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
                                 ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
                                 ICC_DATE_CLASSES | ICC_LINK_CLASS };   // CAPS-7: time picker, SysLink
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_magLock);
    LoadCursorSettings();
    LoadThemeSettings();   // CAPS-7
    // Якщо попередній запуск обірвався із збільшеним курсором — повертаємо розмір
    // ДО того, як щось показуємо користувачу.
    RecoverCursorSize();

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

    WNDCLASSW ov = {};
    ov.lpfnWndProc   = DefWindowProcW;
    ov.hInstance     = hInst;
    ov.lpszClassName = L"capslang_overlay";
    RegisterClassW(&ov);

    const int w = sc(470), h = sc(520);
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

    // Таб-контрол створюємо ПЕРШИМ: сторінки-діти, створені після нього,
    // опиняються вище за z-order і малюються поверх його полотна.
    g_tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           sc(12), sc(12), sc(446), sc(440),
                           hwnd, (HMENU)(INT_PTR)IDC_TABS, hInst, nullptr);
    SendMessageW(g_tabs, WM_SETFONT, (WPARAM)font, TRUE);
    TCITEMW tab = {};
    tab.mask = TCIF_TEXT;
    tab.pszText = (LPWSTR)L"Розкладка";
    SendMessageW(g_tabs, TCM_INSERTITEMW, 0, (LPARAM)&tab);
    tab.pszText = (LPWSTR)L"Курсор";
    SendMessageW(g_tabs, TCM_INSERTITEMW, 1, (LPARAM)&tab);
    tab.pszText = (LPWSTR)L"День/ніч";
    SendMessageW(g_tabs, TCM_INSERTITEMW, 2, (LPARAM)&tab);

    auto addL = [&](HWND c) { g_pageLayout[g_pageLayoutN++] = c; return c; };
    auto addC = [&](HWND c) { g_pageCursor[g_pageCursorN++] = c; return c; };
    auto addA = [&](HWND c) { g_advCtrls[g_advN++] = c; return c; };
    auto addT = [&](HWND c) { g_pageTheme[g_pageThemeN++] = c; return c; };
    auto addTA = [&](HWND c) { g_thAdv[g_thAdvN++] = c; return c; };

    // ---- вкладка «Розкладка» ----
    addL(mk(L"STATIC", L"CapsLock — перемкнути розкладку", 0, 28, 52, 300, 20, 0));
    addL(mk(L"STATIC", L"Shift + CapsLock — звичайний Caps Lock", 0, 28, 76, 300, 20, 0));
    g_checkbox = addL(mk(L"BUTTON", L"Запускати при вході в Windows",
                         BS_AUTOCHECKBOX | WS_TABSTOP, 28, 110, 300, 24, IDC_AUTOSTART));

    addL(mk(L"STATIC", L"Режим роботи:", 0, 28, 148, 200, 20, 0));
    addL(mk(L"BUTTON", L"Основний", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
            28, 172, 130, 22, IDC_MODE_HOOK));
    addL(mk(L"BUTTON", L"Запасний", BS_AUTORADIOBUTTON,
            168, 172, 130, 22, IDC_MODE_HOTKEY));
    g_modeHint = addL(mk(L"STATIC", L"", 0, 28, 200, 410, 20, IDC_MODE_HINT));

    g_passthrough = LoadPassthrough();
    g_passthroughCheckbox = addL(mk(L"BUTTON",
        L"Не перехоплювати Caps Lock при роботі з віртуальними та віддаленими машинами",
        BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 28, 232, 410, 38, IDC_PASSTHROUGH));
    SendMessageW(g_passthroughCheckbox, BM_SETCHECK,
                 g_passthrough ? BST_CHECKED : BST_UNCHECKED, 0);
    addL(mk(L"STATIC", L"Remote Desktop, Windows App, VMware, Hyper-V.",
            0, 28, 274, 410, 18, IDC_PASSTHROUGH_HINT));
    addL(mk(L"STATIC", L"Вікно можна закрити — програма лишається в треї.",
            0, 28, 340, 410, 18, IDC_HINT_GRAY));

    // ---- вкладка «Курсор» ----
    g_curEnable = addC(mk(L"BUTTON", L"Збільшувати курсор, якщо потрусити мишею",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 28, 52, 410, 24, IDC_CUR_ENABLE));
    SendMessageW(g_curEnable, BM_SETCHECK, g_cur.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    addC(mk(L"STATIC", L"Не працює в іграх та інших повноекранних програмах.",
            0, 28, 78, 410, 18, IDC_HINT_GRAY));

    addC(mk(L"STATIC", L"Наскільки збільшувати", 0, 28, 110, 260, 20, 0));
    g_curScaleVal = addC(mk(L"STATIC", L"", SS_RIGHT, 350, 110, 88, 20, 0));
    g_curScale = addC(mk(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP,
                         24, 130, 414, 30, IDC_CUR_SCALE));
    SendMessageW(g_curScale, TBM_SETRANGE, TRUE, MAKELPARAM(2, 8));
    SendMessageW(g_curScale, TBM_SETPOS, TRUE, g_cur.scale);

    addC(mk(L"STATIC", L"Скільки тримати збільшеним", 0, 28, 172, 260, 20, 0));
    g_curHoldVal = addC(mk(L"STATIC", L"", SS_RIGHT, 350, 172, 88, 20, 0));
    g_curHold = addC(mk(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP,
                        24, 192, 414, 30, IDC_CUR_HOLD));
    SendMessageW(g_curHold, TBM_SETRANGE, TRUE, MAKELPARAM(5, 50));
    SendMessageW(g_curHold, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(g_curHold, TBM_SETPOS, TRUE, g_cur.holdMs / 100);

    g_curOverlay = addC(mk(L"BUTTON", L"Зменшувати плавно (намальованою копією)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 28, 230, 410, 24, IDC_CUR_OVERLAY));
    SendMessageW(g_curOverlay, BM_SETCHECK, g_cur.overlay ? BST_CHECKED : BST_UNCHECKED, 0);
    addC(mk(L"STATIC", L"Інакше зменшує сам системний курсор — помітними стрибками.",
            0, 28, 256, 410, 18, IDC_HINT_GRAY));

    g_curAdvBtn = addC(mk(L"BUTTON", L"Детально ▾", BS_PUSHBUTTON | WS_TABSTOP,
                          28, 288, 130, 26, IDC_CUR_ADVANCED));

    // «Детально»: чутливість жесту. Значення приймаються при втраті фокуса й
    // притискаються до робочого діапазону, щоб не можна було вимкнути фічу
    // випадковим нулем.
    auto advRow = [&](const wchar_t* label, int y, int id, int value) {
        addA(mk(L"STATIC", label, 0, 28, y + 3, 250, 18, 0));
        HWND e = addA(mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP,
                         300, y, 80, 22, id));
        wchar_t buf[16];
        wsprintfW(buf, L"%d", value);
        SetWindowTextW(e, buf);
        return e;
    };
    g_edWindow = advRow(L"Вікно розпізнавання жесту, мс", 322, IDC_CUR_WINDOWMS,  g_cur.windowMs);
    g_edDist   = advRow(L"Мінімальний шлях миші, px",     348, IDC_CUR_DIST,      g_cur.distance);
    g_edFactor = advRow(L"Поріг «шлях / розмах», %",      374, IDC_CUR_FACTOR,    g_cur.factor);
    g_edRevers = advRow(L"Мінімум змін напрямку",         400, IDC_CUR_REVERSALS, g_cur.reversals);
    g_edShrink = advRow(L"Тривалість зменшення, мс",      426, IDC_CUR_SHRINK,    g_cur.shrinkMs);

    SetCursorValueLabels();

    // ---- вкладка «День/ніч» (CAPS-7) ----
    g_thEnable = addT(mk(L"BUTTON", L"Автоматично перемикати світлу і темну тему Windows",
                         BS_AUTOCHECKBOX | WS_TABSTOP, 28, 52, 410, 24, IDC_TH_ENABLE));
    SendMessageW(g_thEnable, BM_SETCHECK, g_th.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    g_thBySun = addT(mk(L"BUTTON", L"За сходом і заходом сонця",
                        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, 84, 220, 22, IDC_TH_BY_SUN));
    g_thBySched = addT(mk(L"BUTTON", L"За розкладом", BS_AUTORADIOBUTTON,
                          262, 84, 176, 22, IDC_TH_BY_SCHED));
    CheckRadioButton(hwnd, IDC_TH_BY_SUN, IDC_TH_BY_SCHED,
                     g_th.bySchedule ? IDC_TH_BY_SCHED : IDC_TH_BY_SUN);
    g_thStatus = addT(mk(L"STATIC", L"", 0, 28, 112, 410, 36, IDC_TH_STATUS));

    addT(mk(L"STATIC", L"Темна тема з", 0, 28, 158, 110, 20, 0));
    g_thDarkFrom = addT(mk(DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP,
                           140, 155, 90, 24, IDC_TH_DARK_FROM));
    addT(mk(L"STATIC", L"світла з", 0, 262, 158, 74, 20, 0));
    g_thLightFrom = addT(mk(DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP,
                            348, 155, 90, 24, IDC_TH_LIGHT_FROM));
    SetPickerMinutes(g_thDarkFrom,  g_th.darkFrom);
    SetPickerMinutes(g_thLightFrom, g_th.lightFrom);

    g_thToggle = addT(mk(L"BUTTON", L"Переключити зараз", BS_PUSHBUTTON | WS_TABSTOP,
                         28, 196, 170, 26, IDC_TH_TOGGLE));
    g_thNow = addT(mk(L"STATIC", L"", 0, 28, 230, 410, 36, IDC_TH_NOW));
    addT(mk(L"STATIC", L"Поки відкрита повноекранна програма, тема не змінюється — "
                       L"перемкнеться після її закриття.", 0, 28, 268, 410, 34, IDC_HINT_GRAY));
    g_thAdvBtn = addT(mk(L"BUTTON", L"Детально ▾", BS_PUSHBUTTON | WS_TABSTOP,
                         28, 310, 130, 26, IDC_TH_ADVANCED));

    // «Детально»: звідки брати розташування для сходу/заходу
    addTA(mk(L"STATIC", L"Розташування для сходу/заходу:", 0, 28, 346, 410, 18, 0));
    {
        const wchar_t* names[5] = { L"Автоматично", L"Служба Windows", L"За IP-адресою",
                                    L"Вручну", L"Часовий пояс і регіон" };
        const int xs[5] = { 28, 150, 290, 28, 150 };
        const int ys[5] = { 366, 366, 366, 388, 388 };
        const int ws[5] = { 116, 134, 148, 116, 210 };
        for (int i = 0; i < 5; ++i)
            g_thSrc[i] = addTA(mk(L"BUTTON", names[i],
                BS_AUTORADIOBUTTON | (i == 0 ? (WS_GROUP | WS_TABSTOP) : 0),
                xs[i], ys[i], ws[i], 20, IDC_TH_SRC_AUTO + i));
        CheckRadioButton(hwnd, IDC_TH_SRC_AUTO, IDC_TH_SRC_TZ, IDC_TH_SRC_AUTO + (int)g_th.src);
    }
    addTA(mk(L"STATIC", L"Широта", 0, 28, 416, 60, 18, 0));
    g_thLat = addTA(mk(L"EDIT", L"", ES_RIGHT | WS_BORDER | WS_TABSTOP, 92, 413, 90, 22, IDC_TH_LAT));
    addTA(mk(L"STATIC", L"Довгота", 0, 200, 416, 64, 18, 0));
    g_thLon = addTA(mk(L"EDIT", L"", ES_RIGHT | WS_BORDER | WS_TABSTOP, 268, 413, 90, 22, IDC_TH_LON));
    if (g_th.hasManual) {
        wchar_t b[32];
        swprintf(b, 32, L"%.4f", g_th.lat); SetWindowTextW(g_thLat, b);
        swprintf(b, 32, L"%.4f", g_th.lon); SetWindowTextW(g_thLon, b);
    }

    // Підвал: © + версія з VERSIONINFO + посилання (CAPS-7)
    {
        wchar_t ver[32] = {}, about[192] = {};
        ExeVersionString(ver, 32);
        swprintf(about, 192, L"© Plum, 2026 · v%s · <a href=\"https://github.com/V-Plum/capslang\">GitHub</a>", ver);
        mk(L"SysLink", about, 0, 20, 464, 300, 18, IDC_COPYRIGHT);   // WC_LINK
    }

    // Логотип — поза вкладками, інакше його перекриє полотно таб-контрола
    SetRect(&g_logoRect, sc(398), sc(456), sc(398 + 48), sc(456 + 48));

    SelectTab(0);

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

    // CAPS-7: одразу привести тему до часу доби; координати — з кешу, свіжі у фоні.
    EnableThemeControls();
    if (g_th.enabled) {
        SetTimer(hwnd, TIMER_THEME, 60 * 1000, nullptr);
        if (!g_th.bySchedule && (!g_fix.ok || NowUnix() - g_fix.at > 6 * 3600))
            StartLocate();
        ThemeTick();
    } else {
        UpdateThemeStatus();
    }

    // CAPS-1: стежимо за зміною активного вікна, щоб знати, коли ми в remote/VM.
    g_inRemote = IsRemoteWindow(GetForegroundWindow());
    g_winEvent = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                 nullptr, WinEventProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    StartHookThread();  // має бути до StartInterception у режимі Hook
    ApplyCursorFeature();  // CAPS-2: мишачий хук на тому ж потоці
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

    MagnifyRestore();   // страховка, якщо цикл завершився повз WM_DESTROY
    DeleteCriticalSection(&g_magLock);
    StopInterception();
    StopHookThread();
    if (g_winEvent) UnhookWinEvent(g_winEvent);
    delete g_logo;
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return 0;
}
