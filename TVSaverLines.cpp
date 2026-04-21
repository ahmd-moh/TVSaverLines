// TVSaverLines.cpp  (v12)
// -----------------------------------------------------------------------------
// Secondary-monitor (TV) burn-in screen-saver
//
// What's new vs v10:
//   • FIXED: Magenta color-key conflict (was invisible as H-line) →
//             transparent key is now RGB(2,2,2); Magenta → Purple in palette
//   • 5 vertical-line color schemas (Full Spectrum / Cool / Warm / Soft / Single)
//   • Bidirectional sweep option (alternates L→R and R→L each pass)
//   • Smooth fade-in / fade-out (1.5 s each end — polite, no pop-in)
//   • Schema + bothDirs forwarded to test-preview subprocess
//   • Line Opacity removed from dialog — LWA_COLORKEY+LWA_ALPHA quirk makes
//     sub-100% values show a dark rectangle over screen; lines now always
//     run at full alpha=255 (fade-in/out still goes 0→255 cleanly)
//
// Launch modes:
//   TVSaverLines.exe               → Configuration dialog
//   TVSaverLines.exe /run          → One animation pass on secondary monitor
//   TVSaverLines.exe /test 1|2     → Preview on main(1) or second(2) screen
//       /color N    override color index
//       /schema N   override schema index
//       /single     vertical line only
//       /both       both lines
//       /bothdirs   alternate sweep direction
//
// Compile (MSVC, x64 Native Tools prompt):
//   cl /EHsc /O2 /DUNICODE /D_UNICODE TVSaverLines.cpp ^
//      user32.lib gdi32.lib shell32.lib ^
//      /link /SUBSYSTEM:WINDOWS
//
// Compile (MinGW-w64):
//   g++ -O2 -municode -mwindows TVSaverLines.cpp -o TVSaverLines.exe ^
//       -lgdi32 -lshell32
// -----------------------------------------------------------------------------

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#ifdef _MSC_VER
#pragma comment(linker, \
  "\"/manifestdependency:type='win32' "                                        \
  "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "                \
  "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// ============================================================================
// Tunables
// ============================================================================
static const int LINE_THICKNESS_PCT = 4;
static const int LINE_THICKNESS_MIN = 15;
// SWEEP_PERIOD_MS is now user-configurable (stored in AnimState.sweepPeriodMs)
static const int FRAME_DELAY_MS     = 16;
static const int TEST_DURATION_MS   = 12000;
static const int FADE_MS            = 1500;   // fade-in and fade-out duration

// ============================================================================
// Transparency key — must NOT appear in any band or palette color
// v10 bug: was RGB(255,0,255) which equalled Magenta → Magenta line was invisible
// ============================================================================
static const COLORREF kTransparentKey = RGB(2, 2, 2);

// ============================================================================
// Horizontal-line color palette
// ============================================================================
struct ColorEntry { const wchar_t* name; COLORREF rgb; };
static const ColorEntry kColors[] = {
    { L"Red",    RGB(255,   0,   0) },
    { L"Orange", RGB(255, 140,   0) },
    { L"Yellow", RGB(255, 255,   0) },
    { L"Green",  RGB(  0, 200,   0) },
    { L"Cyan",   RGB(  0, 255, 255) },
    { L"Blue",   RGB( 30, 144, 255) },
    { L"Purple", RGB(128,   0, 128) },   // replaces Magenta (was invisible due to v10 bug)
    { L"White",  RGB(255, 255, 255) },
    { L"Pink",   RGB(255, 105, 180) },
    { L"Lime",   RGB(160, 255,  50) },
};
static const int kColorCount = (int)(sizeof(kColors) / sizeof(kColors[0]));

// ============================================================================
// Vertical-line color schemas
// ============================================================================
struct VBand { COLORREF color; int weight; };

// Max 5 bands per schema; unused slots are zero-init (weight=0, never drawn)
struct VSchema {
    const wchar_t* name;
    VBand bands[5];
    int   count;      // number of active bands
    int   totalW;     // sum of weights
};

// Full Spectrum is the best for actual burn-in prevention:
// sweeps Black→Red→Green→Blue→White → every subpixel type is exercised.
static const VSchema kSchemas[] = {
    {
        L"Full Spectrum  (Black + R + G + B + White)",
        {
            { RGB(  0,   0,   0), 2 },
            { RGB(255,   0,   0), 1 },
            { RGB(  0, 255,   0), 1 },
            { RGB(  0,   0, 255), 1 },
            { RGB(255, 255, 255), 2 },
        },
        5, 7
    },
    {
        L"Cool  (Black + Blue + Cyan + White)",
        {
            { RGB(  0,   0,   0), 1 },
            { RGB( 30, 144, 255), 2 },
            { RGB(  0, 255, 255), 2 },
            { RGB(255, 255, 255), 2 },
            { RGB(  0,   0,   0), 0 },  // unused
        },
        4, 7
    },
    {
        L"Warm  (Black + Red + Orange + Yellow)",
        {
            { RGB(  0,   0,   0), 1 },
            { RGB(255,   0,   0), 2 },
            { RGB(255, 140,   0), 2 },
            { RGB(255, 220,   0), 2 },
            { RGB(  0,   0,   0), 0 },  // unused
        },
        4, 7
    },
    {
        L"Soft  (Black + Purple + Pink + White)",
        {
            { RGB(  0,   0,   0), 1 },
            { RGB(128,   0, 128), 2 },
            { RGB(255, 105, 180), 2 },
            { RGB(255, 255, 255), 2 },
            { RGB(  0,   0,   0), 0 },  // unused
        },
        4, 7
    },
    {
        L"Single Color  (same as H-line color above)",
        { {0,0},{0,0},{0,0},{0,0},{0,0} },
        0, 1    // special: drawn as one solid band using the chosen H-line color
    },
};
static const int kSchemaCount = (int)(sizeof(kSchemas) / sizeof(kSchemas[0]));

// ============================================================================
// Configuration  (persisted in %APPDATA%\TVSaverLines\config.ini)
// ============================================================================
struct Config {
    int  intervalHours;   // 2 4 6 8 10
    int  colorIndex;      // horizontal line color (and V-line when schema = Single)
    bool singleLine;      // false = both lines;  true = vertical only
    bool bothDirs;        // false = L→R only;    true = alternate L↔R each pass
    int  schemaIndex;     // 0 .. kSchemaCount-1
    int  runDurationSec;  // 10 20 30 60 120
    int  sweepPeriodMs;   // ms per full sweep: 12000 10000 7000 4000 2000

    Config()
        : intervalHours(4), colorIndex(0), singleLine(true),
          bothDirs(true), schemaIndex(0),
          runDurationSec(30), sweepPeriodMs(7000)
    {}
};

static void GetIniPath(wchar_t* out, size_t cap)
{
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0) {
        swprintf_s(out, cap, L"%s\\TVSaverLines", appdata);
        CreateDirectoryW(out, nullptr);
        wcscat_s(out, cap, L"\\config.ini");
    } else {
        GetModuleFileNameW(nullptr, out, (DWORD)cap);
        for (int i = (int)wcslen(out) - 1; i >= 0; --i)
            if (out[i] == L'\\') { out[i+1] = 0; break; }
        wcscat_s(out, cap, L"config.ini");
    }
}

static Config LoadConfig()
{
    wchar_t ini[MAX_PATH]; GetIniPath(ini, MAX_PATH);
    Config c;
    c.intervalHours  = (int)GetPrivateProfileIntW(L"Settings", L"IntervalHours",   4, ini);
    c.colorIndex     = (int)GetPrivateProfileIntW(L"Settings", L"ColorIndex",       0, ini);
    c.singleLine     = GetPrivateProfileIntW(L"Settings", L"SingleLine",   1, ini) != 0;
    c.bothDirs       = GetPrivateProfileIntW(L"Settings", L"BothDirs",     1, ini) != 0;
    c.schemaIndex    = (int)GetPrivateProfileIntW(L"Settings", L"SchemaIndex",      0, ini);
    c.runDurationSec = (int)GetPrivateProfileIntW(L"Settings", L"RunDurationSec",  30, ini);
    c.sweepPeriodMs  = (int)GetPrivateProfileIntW(L"Settings", L"SweepPeriodMs", 7000, ini);

    if (c.intervalHours < 2 || c.intervalHours > 10 || (c.intervalHours & 1))
        c.intervalHours = 4;
    if (c.colorIndex  < 0 || c.colorIndex  >= kColorCount)  c.colorIndex  = 0;
    if (c.schemaIndex < 0 || c.schemaIndex >= kSchemaCount) c.schemaIndex = 0;
    if (c.runDurationSec < 10 || c.runDurationSec > 300)    c.runDurationSec = 30;
    if (c.sweepPeriodMs  < 500 || c.sweepPeriodMs > 30000)  c.sweepPeriodMs  = 7000;
    return c;
}

static void SaveConfig(const Config& c)
{
    wchar_t ini[MAX_PATH]; GetIniPath(ini, MAX_PATH);
    wchar_t buf[32];

    swprintf_s(buf, L"%d", c.intervalHours);
    WritePrivateProfileStringW(L"Settings", L"IntervalHours", buf, ini);
    swprintf_s(buf, L"%d", c.colorIndex);
    WritePrivateProfileStringW(L"Settings", L"ColorIndex", buf, ini);
    swprintf_s(buf, L"%d", c.singleLine ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"SingleLine", buf, ini);
    swprintf_s(buf, L"%d", c.bothDirs ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"BothDirs", buf, ini);
    swprintf_s(buf, L"%d", c.schemaIndex);
    WritePrivateProfileStringW(L"Settings", L"SchemaIndex", buf, ini);
    swprintf_s(buf, L"%d", c.runDurationSec);
    WritePrivateProfileStringW(L"Settings", L"RunDurationSec", buf, ini);
    swprintf_s(buf, L"%d", c.sweepPeriodMs);
    WritePrivateProfileStringW(L"Settings", L"SweepPeriodMs", buf, ini);
}

// ============================================================================
// Monitor enumeration
// ============================================================================
struct MonCtx { bool wantPrimary; RECT out; bool found; };

static BOOL CALLBACK MonEnumProc(HMONITOR hm, HDC, LPRECT, LPARAM lp)
{
    MonCtx* ctx = (MonCtx*)lp;
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hm, &mi)) {
        bool isP = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        if (ctx->wantPrimary == isP) {
            ctx->out = mi.rcMonitor; ctx->found = true; return FALSE;
        }
    }
    return TRUE;
}

static bool GetMonitorRect(bool primary, RECT* out)
{
    MonCtx ctx = { primary, {0,0,0,0}, false };
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&ctx);
    if (ctx.found) { *out = ctx.out; return true; }
    return false;
}

// ============================================================================
// Animation window
// ============================================================================
enum { TID_FRAME = 1 };

struct AnimState {
    RECT     rect;
    bool     singleLine;
    bool     bothDirs;
    int      schemaIndex;
    COLORREF hColor;        // color for horizontal line (and V-single)
    int      durationMs;
    int      sweepPeriodMs;  // ms per full sweep cycle
    DWORD    startTime;
    int      xPos, yPos;
    int      thickH, thickV;
    bool     testMode;
    BYTE     baseAlpha;     // always 255; fade-in/out multiplies downward from this
    BYTE     curAlpha;      // last alpha applied (to avoid redundant API calls)
    bool     finishReq;     // duration elapsed, fade-out started
    DWORD    finishTime;    // tick count when finishReq became true
};
static AnimState g_anim;

// Only calls SetLayeredWindowAttributes when the value actually changes
static void ApplyAlpha(HWND hWnd, BYTE a)
{
    if (a == g_anim.curAlpha) return;
    g_anim.curAlpha = a;
    SetLayeredWindowAttributes(hWnd, kTransparentKey, a, LWA_COLORKEY | LWA_ALPHA);
}

static LRESULT CALLBACK AnimProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;

    case WM_TIMER:
        if (w == TID_FRAME) {
            DWORD el = GetTickCount() - g_anim.startTime;

            // --- Duration check → begin fade-out ---
            if (!g_anim.finishReq && (int)el >= g_anim.durationMs) {
                g_anim.finishReq  = true;
                g_anim.finishTime = GetTickCount();
            }

            // --- Fade logic ---
            if (g_anim.finishReq) {
                DWORD since = GetTickCount() - g_anim.finishTime;
                if ((int)since >= FADE_MS) {
                    DestroyWindow(hWnd);
                    return 0;
                }
                float f = 1.0f - (float)since / (float)FADE_MS;
                ApplyAlpha(hWnd, (BYTE)((float)g_anim.baseAlpha * f));
            } else {
                // Fade-in during first FADE_MS
                float f = (float)el / (float)FADE_MS;
                if (f > 1.0f) f = 1.0f;
                ApplyAlpha(hWnd, (BYTE)((float)g_anim.baseAlpha * f));
            }

            // --- Position (with optional bidirectional sweep) ---
            int mw = g_anim.rect.right  - g_anim.rect.left;
            int mh = g_anim.rect.bottom - g_anim.rect.top;
            float t = (float)(el % g_anim.sweepPeriodMs) / (float)g_anim.sweepPeriodMs;

            // Reverse direction on odd-numbered sweep cycles when bothDirs is set
            if (g_anim.bothDirs && ((el / (DWORD)g_anim.sweepPeriodMs) % 2 == 1))
                t = 1.0f - t;

            g_anim.xPos = (int)(t * (float)(mw - g_anim.thickV));
            g_anim.yPos = (int)(t * (float)(mh - g_anim.thickH));

            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        if (g_anim.testMode && w == VK_ESCAPE && !g_anim.finishReq) {
            g_anim.finishReq  = true;
            g_anim.finishTime = GetTickCount();
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        // --- Double-buffer ---
        HDC     mdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mdc, bmp);

        // Fill with transparent color key (will be keyed out by layered window)
        HBRUSH hbk = CreateSolidBrush(kTransparentKey);
        FillRect(mdc, &rc, hbk);
        DeleteObject(hbk);

        // === Vertical line ===
        if (g_anim.schemaIndex >= 0 && g_anim.schemaIndex < kSchemaCount - 1) {
            // Multi-band schema
            const VSchema& s = kSchemas[g_anim.schemaIndex];
            int x   = g_anim.xPos;
            int rem = g_anim.thickV;

            for (int i = 0; i < s.count && rem > 0; i++) {
                int bw;
                if (i == s.count - 1) {
                    bw = rem;   // last band absorbs rounding remainder
                } else {
                    bw = (g_anim.thickV * s.bands[i].weight) / s.totalW;
                    if (bw < 1) bw = 1;
                    if (bw > rem) bw = rem;
                }
                rem -= bw;

                HBRUSH b = CreateSolidBrush(s.bands[i].color);
                RECT vr = { x, 0, x + bw, rc.bottom };
                FillRect(mdc, &vr, b);
                DeleteObject(b);
                x += bw;
            }
        } else {
            // Single-color vertical line (schema index == kSchemaCount-1)
            HBRUSH b = CreateSolidBrush(g_anim.hColor);
            RECT vr = { g_anim.xPos, 0, g_anim.xPos + g_anim.thickV, rc.bottom };
            FillRect(mdc, &vr, b);
            DeleteObject(b);
        }

        // === Horizontal line (when both-lines mode is on) ===
        if (!g_anim.singleLine) {
            HBRUSH b = CreateSolidBrush(g_anim.hColor);
            RECT hr = { 0, g_anim.yPos, rc.right, g_anim.yPos + g_anim.thickH };
            FillRect(mdc, &hr, b);
            DeleteObject(b);
        }

        // Test-mode hint
        if (g_anim.testMode) {
            SetBkMode(mdc, TRANSPARENT);
            SetTextColor(mdc, RGB(230, 230, 230));
            const wchar_t* hint = L"TEST MODE  \u2013  press ESC to close";
            TextOutW(mdc, 24, 24, hint, (int)wcslen(hint));
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, w, l);
}

static int RunAnimation(bool usePrimary, int durationMs, bool testMode, const Config& c)
{
    RECT r;
    if (!GetMonitorRect(usePrimary, &r)) {
        if (testMode)
            MessageBoxW(nullptr,
                usePrimary
                    ? L"No primary monitor found."
                    : L"No secondary monitor detected.\n"
                      L"Connect your TV in extended-display mode and try again.",
                L"TVSaverLines", MB_ICONWARNING | MB_OK);
        return 0;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = AnimProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"TVSaverAnimClass";
    RegisterClassExW(&wc);

    int w = r.right - r.left, h = r.bottom - r.top;

    int tH = h * LINE_THICKNESS_PCT / 100;
    if (tH < LINE_THICKNESS_MIN) tH = LINE_THICKNESS_MIN;
    int tV = w * LINE_THICKNESS_PCT / 100;
    if (tV < LINE_THICKNESS_MIN) tV = LINE_THICKNESS_MIN;

    g_anim.rect        = r;
    g_anim.singleLine  = c.singleLine;
    g_anim.bothDirs    = c.bothDirs;
    g_anim.schemaIndex = c.schemaIndex;
    g_anim.hColor      = kColors[c.colorIndex].rgb;
    g_anim.durationMs    = durationMs;
    g_anim.sweepPeriodMs = c.sweepPeriodMs;
    g_anim.startTime   = GetTickCount();
    g_anim.xPos        = 0;
    g_anim.yPos        = 0;
    g_anim.thickH      = tH;
    g_anim.thickV      = tV;
    g_anim.testMode    = testMode;
    g_anim.baseAlpha   = 255;   // always full; opacity knob removed (LWA_COLORKEY+LWA_ALPHA quirk)
    g_anim.curAlpha    = 0;
    g_anim.finishReq   = false;
    g_anim.finishTime  = 0;

    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
    if (!testMode) exStyle |= WS_EX_NOACTIVATE;

    HWND hWnd = CreateWindowExW(exStyle,
        L"TVSaverAnimClass", L"TVSaverAnim",
        WS_POPUP, r.left, r.top, w, h,
        nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 1;

    // Start fully transparent; the timer will fade it in smoothly
    SetLayeredWindowAttributes(hWnd, kTransparentKey, 0, LWA_COLORKEY | LWA_ALPHA);

    ShowWindow(hWnd, testMode ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (testMode) SetForegroundWindow(hWnd);
    SetTimer(hWnd, TID_FRAME, FRAME_DELAY_MS, nullptr);

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}

// ============================================================================
// Task Scheduler (via schtasks.exe)
// ============================================================================
static bool RunSchtasks(wchar_t* cmdLine, DWORD& exitCode)
{
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

static bool CreateScheduledTask(int hours, HWND parent)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    // Start time = now + 1 minute (so first run happens very soon after Save)
    SYSTEMTIME st; GetLocalTime(&st);
    int totalMin = st.wHour * 60 + st.wMinute + 1;
    int hh = (totalMin / 60) % 24;
    int mm =  totalMin % 60;

    wchar_t cmd[4096];
    swprintf_s(cmd,
        L"schtasks.exe /Create /TN \"TVSaverLines\" "
        L"/TR \"\\\"%s\\\" /run\" "
        L"/SC HOURLY /MO %d /ST %02d:%02d /F",
        exe, hours, hh, mm);

    DWORD rc = 1;
    if (!RunSchtasks(cmd, rc) || rc != 0) {
        MessageBoxW(parent,
            L"Settings were saved, but the scheduled task could not be created.\n"
            L"You can create it manually in Task Scheduler.",
            L"TVSaverLines \u2013 Warning", MB_ICONWARNING | MB_OK);
        return false;
    }

    wchar_t msg[640];
    swprintf_s(msg,
        L"Settings saved.\n\n"
        L"Task Scheduler entry \"TVSaverLines\" created.\n"
        L"  First run : today at %02d:%02d\n"
        L"  Repeats   : every %d hour(s)\n"
        L"  Command   : \"%s\" /run\n\n"
        L"Manage it from Task Scheduler if needed.",
        hh, mm, hours, exe);
    MessageBoxW(parent, msg, L"TVSaverLines", MB_ICONINFORMATION | MB_OK);
    return true;
}

// ============================================================================
// Configuration dialog
// ============================================================================
#define IDC_INTERVAL   1001
#define IDC_COLOR      1002
#define IDC_SCHEMA     1003
#define IDC_LINEMODE_BOTH   1004  // radio: both lines
#define IDC_LINEMODE_SINGLE 1005  // radio: vertical only
#define IDC_BOTHDIRS   1006       // checkbox: alternate direction
#define IDC_TESTWHERE  1007
#define IDC_TEST       1008
#define IDC_SAVE       1009
#define IDC_CANCEL     1010
#define IDC_DURATION   1011
#define IDC_SPEED      1012

static HFONT g_hFont = nullptr;
static int   g_dpi   = 96;

// Scale a logical (96-dpi) pixel count to the actual DPI
static int DS(int v) { return MulDiv(v, g_dpi, 96); }

static HWND MakeCtrl(HWND parent, LPCWSTR cls, LPCWSTR text, DWORD style,
                     int x, int y, int cw, int ch, int id)
{
    HWND h = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                             DS(x), DS(y), DS(cw), DS(ch),
                             parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (h && g_hFont) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return h;
}

// Read current dialog state and spawn a test-preview process
static void LaunchTestProcess(HWND hWnd)
{
    int sel   = (int)SendMessageW(GetDlgItem(hWnd, IDC_TESTWHERE), CB_GETCURSEL, 0, 0);
    int clr   = (int)SendMessageW(GetDlgItem(hWnd, IDC_COLOR),     CB_GETCURSEL, 0, 0);
    int sch   = (int)SendMessageW(GetDlgItem(hWnd, IDC_SCHEMA),    CB_GETCURSEL, 0, 0);
    int spd   = (int)SendMessageW(GetDlgItem(hWnd, IDC_SPEED),     CB_GETCURSEL, 0, 0);
    bool sln  = IsDlgButtonChecked(hWnd, IDC_LINEMODE_SINGLE) == BST_CHECKED;
    bool bdir = IsDlgButtonChecked(hWnd, IDC_BOTHDIRS)        == BST_CHECKED;
    if (clr < 0) clr = 0;
    if (sch < 0) sch = 0;

    static const int spdVals[] = { 12000, 10000, 7000, 4000, 2000 };
    int spdMs = (spd >= 0 && spd < 5) ? spdVals[spd] : 7000;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    wchar_t args[512];
    swprintf_s(args, L"/test %d /color %d /schema %d /speed %d %s%s",
               (sel == 0) ? 1 : 2,
               clr, sch, spdMs,
               sln  ? L"/single "  : L"/both ",
               bdir ? L"/bothdirs" : L"");

    ShellExecuteW(nullptr, L"open", exe, args, nullptr, SW_SHOWNORMAL);
}

static LRESULT CALLBACK ConfigProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_COMMAND: {
        int id = LOWORD(w);

        if (id == IDC_TEST)   { LaunchTestProcess(hWnd); return 0; }
        if (id == IDC_CANCEL) { DestroyWindow(hWnd);     return 0; }

        if (id == IDC_SAVE) {
            static const int durVals[] = {10, 20, 30, 60, 120};

            Config c;
            c.intervalHours = 2 + 2 * (int)SendMessageW(
                GetDlgItem(hWnd, IDC_INTERVAL), CB_GETCURSEL, 0, 0);
            c.colorIndex    = (int)SendMessageW(
                GetDlgItem(hWnd, IDC_COLOR),    CB_GETCURSEL, 0, 0);
            c.schemaIndex   = (int)SendMessageW(
                GetDlgItem(hWnd, IDC_SCHEMA),   CB_GETCURSEL, 0, 0);
            c.singleLine    = IsDlgButtonChecked(hWnd, IDC_LINEMODE_SINGLE) == BST_CHECKED;
            c.bothDirs      = IsDlgButtonChecked(hWnd, IDC_BOTHDIRS)        == BST_CHECKED;

            int durIdx = (int)SendMessageW(GetDlgItem(hWnd, IDC_DURATION), CB_GETCURSEL, 0, 0);
            c.runDurationSec = (durIdx >= 0 && durIdx < 5) ? durVals[durIdx] : 30;

            {
                static const int spdVals[] = { 12000, 10000, 7000, 4000, 2000 };
                int spdIdx = (int)SendMessageW(GetDlgItem(hWnd, IDC_SPEED), CB_GETCURSEL, 0, 0);
                c.sweepPeriodMs = (spdIdx >= 0 && spdIdx < 5) ? spdVals[spdIdx] : 7000;
            }

            // Guard against bad combo-box state
            if (c.intervalHours < 2 || c.intervalHours > 10) c.intervalHours = 4;
            if (c.colorIndex  < 0 || c.colorIndex  >= kColorCount)  c.colorIndex  = 0;
            if (c.schemaIndex < 0 || c.schemaIndex >= kSchemaCount)  c.schemaIndex = 0;

            SaveConfig(c);
            if (CreateScheduledTask(c.intervalHours, hWnd))
                DestroyWindow(hWnd);
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:   DestroyWindow(hWnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProc(hWnd, msg, w, l);
}

static int ShowConfigDialog(HINSTANCE hInst)
{
    // Query DPI of the primary screen
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = ConfigProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"TVSaverCfgClass";
    RegisterClassExW(&wc);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    // ----------------------------------------------------------------
    // Dialog layout  (all values are logical 96-dpi pixels; DS() scales)
    // ----------------------------------------------------------------
    // Section            y_start  grp_h   y_after
    // Time Activation      10      55       75
    // H-line Color         75      55      140
    // Vertical Schema     140      55      205
    // Line Mode           205      98      315   (3 controls inside)
    // Run Duration        315      55      380
    // Sweep Speed         380      55      445
    // Test Preview        445      55      510
    // Buttons             520
    // CH                  560
    // ----------------------------------------------------------------
    const int gbX = 14, gbW = 432;
    const int CW = 460, CH = 560;

    RECT want = { 0, 0, DS(CW), DS(CH) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&want, style, FALSE, WS_EX_DLGMODALFRAME);
    int W = want.right - want.left, H = want.bottom - want.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME,
        L"TVSaverCfgClass",
        L"TV Saver Lines  \u2013  Configuration",
        style, sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 1;

    int y = 10;

    // --- Time Activation ---
    MakeCtrl(hWnd, L"BUTTON", L"  Time Activation  ", BS_GROUPBOX,
             gbX, y, gbW, 55, -1);
    MakeCtrl(hWnd, L"STATIC", L"Run every:", SS_LEFT,
             gbX+15, y+26, 80, 18, -1);
    HWND cbInt = MakeCtrl(hWnd, L"COMBOBOX", nullptr,
             CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
             gbX+100, y+22, 150, 200, IDC_INTERVAL);
    for (int h = 2; h <= 10; h += 2) {
        wchar_t s[32]; swprintf_s(s, L"%d hours", h);
        SendMessageW(cbInt, CB_ADDSTRING, 0, (LPARAM)s);
    }
    y += 65;

    // --- Horizontal Line Color ---
    MakeCtrl(hWnd, L"BUTTON", L"  Horizontal Line Color  ", BS_GROUPBOX,
             gbX, y, gbW, 55, -1);
    MakeCtrl(hWnd, L"STATIC", L"Color:", SS_LEFT,
             gbX+15, y+26, 55, 18, -1);
    HWND cbClr = MakeCtrl(hWnd, L"COMBOBOX", nullptr,
             CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
             gbX+75, y+22, 185, 240, IDC_COLOR);
    for (int i = 0; i < kColorCount; i++)
        SendMessageW(cbClr, CB_ADDSTRING, 0, (LPARAM)kColors[i].name);
    y += 65;

    // --- Vertical Line Schema ---
    MakeCtrl(hWnd, L"BUTTON", L"  Vertical Line Schema  ", BS_GROUPBOX,
             gbX, y, gbW, 55, -1);
    MakeCtrl(hWnd, L"STATIC", L"Schema:", SS_LEFT,
             gbX+15, y+26, 60, 18, -1);
    HWND cbSch = MakeCtrl(hWnd, L"COMBOBOX", nullptr,
             CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
             gbX+80, y+22, 335, 200, IDC_SCHEMA);
    for (int i = 0; i < kSchemaCount; i++)
        SendMessageW(cbSch, CB_ADDSTRING, 0, (LPARAM)kSchemas[i].name);
    y += 65;

    // --- Line Mode (radio + checkbox) ---
    MakeCtrl(hWnd, L"BUTTON", L"  Line Mode  ", BS_GROUPBOX,
             gbX, y, gbW, 98, -1);
    // Single line on top (default), Both lines below
    MakeCtrl(hWnd, L"BUTTON",
             L"Vertical line only  (left \u2192 right)",
             BS_AUTORADIOBUTTON|WS_GROUP|WS_TABSTOP,
             gbX+15, y+20, 390, 20, IDC_LINEMODE_SINGLE);
    MakeCtrl(hWnd, L"BUTTON",
             L"Both lines  (vertical + horizontal)",
             BS_AUTORADIOBUTTON,
             gbX+15, y+43, 390, 20, IDC_LINEMODE_BOTH);
    MakeCtrl(hWnd, L"BUTTON",
             L"Alternate direction each pass  (left \u2192 right \u2192 left)  \u2713 Recommended",
             BS_AUTOCHECKBOX|WS_TABSTOP,
             gbX+15, y+70, 390, 20, IDC_BOTHDIRS);
    y += 110;

    // --- Run Duration ---
    MakeCtrl(hWnd, L"BUTTON", L"  Run Duration (each pass)  ", BS_GROUPBOX,
             gbX, y, gbW, 55, -1);
    MakeCtrl(hWnd, L"STATIC", L"Duration:", SS_LEFT,
             gbX+15, y+26, 70, 18, -1);
    HWND cbDur = MakeCtrl(hWnd, L"COMBOBOX", nullptr,
             CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
             gbX+90, y+22, 150, 200, IDC_DURATION);
    {
        const wchar_t* dTxt[] = { L"10 seconds", L"20 seconds", L"30 seconds",
                                   L"60 seconds", L"120 seconds" };
        for (int i = 0; i < 5; i++)
            SendMessageW(cbDur, CB_ADDSTRING, 0, (LPARAM)dTxt[i]);
    }
    y += 65;

    // --- Sweep Speed ---
    MakeCtrl(hWnd, L"BUTTON", L"  Sweep Speed  ", BS_GROUPBOX,
             gbX, y, gbW, 55, -1);
    MakeCtrl(hWnd, L"STATIC", L"Speed:", SS_LEFT,
             gbX+15, y+26, 55, 18, -1);
    HWND cbSpd = MakeCtrl(hWnd, L"COMBOBOX", nullptr,
             CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
             gbX+75, y+22, 205, 200, IDC_SPEED);
    {
        const wchar_t* sTxt[] = {
            L"Very Slow  (12 s per sweep)",
            L"Slow       (10 s per sweep)",
            L"Normal     ( 7 s per sweep)",
            L"Fast       ( 4 s per sweep)",
            L"Very Fast  ( 2 s per sweep)",
        };
        for (int i = 0; i < 5; i++)
            SendMessageW(cbSpd, CB_ADDSTRING, 0, (LPARAM)sTxt[i]);
    }
    y += 65;

    // --- Test Preview ---
    MakeCtrl(hWnd, L"BUTTON", L"  Test Preview  ", BS_GROUPBOX,
             gbX, y, gbW, 55, -1);
    MakeCtrl(hWnd, L"STATIC", L"Screen:", SS_LEFT,
             gbX+15, y+26, 55, 18, -1);
    HWND cbTst = MakeCtrl(hWnd, L"COMBOBOX", nullptr,
             CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
             gbX+75, y+22, 195, 120, IDC_TESTWHERE);
    SendMessageW(cbTst, CB_ADDSTRING, 0, (LPARAM)L"Main Screen");
    SendMessageW(cbTst, CB_ADDSTRING, 0, (LPARAM)L"Second Screen (TV)");
    MakeCtrl(hWnd, L"BUTTON", L"Run Test",
             BS_PUSHBUTTON|WS_TABSTOP,
             gbX+285, y+22, 130, 26, IDC_TEST);
    y += 65;

    // --- Save / Cancel ---
    y += 10;
    MakeCtrl(hWnd, L"BUTTON", L"Save && Schedule",
             BS_DEFPUSHBUTTON|WS_TABSTOP,
             gbX+75, y, 180, 32, IDC_SAVE);
    MakeCtrl(hWnd, L"BUTTON", L"Cancel",
             BS_PUSHBUTTON|WS_TABSTOP,
             gbX+270, y, 145, 32, IDC_CANCEL);

    // --- Populate from saved config ---
    Config c = LoadConfig();

    SendMessageW(cbInt, CB_SETCURSEL, (c.intervalHours - 2) / 2, 0);
    SendMessageW(cbClr, CB_SETCURSEL,  c.colorIndex,             0);
    SendMessageW(cbSch, CB_SETCURSEL,  c.schemaIndex,            0);
    SendMessageW(cbTst, CB_SETCURSEL,  1,                        0);  // default: second screen

    {
        static const int durVals[] = {10, 20, 30, 60, 120};
        int durSel = 2;
        for (int i = 0; i < 5; i++)
            if (durVals[i] == c.runDurationSec) { durSel = i; break; }
        SendMessageW(cbDur, CB_SETCURSEL, durSel, 0);
    }

    {
        static const int spdVals[] = { 12000, 10000, 7000, 4000, 2000 };
        int spdSel = 2;   // default: Normal (7000 ms)
        for (int i = 0; i < 5; i++)
            if (spdVals[i] == c.sweepPeriodMs) { spdSel = i; break; }
        SendMessageW(cbSpd, CB_SETCURSEL, spdSel, 0);
    }

    CheckRadioButton(hWnd,
        IDC_LINEMODE_BOTH, IDC_LINEMODE_SINGLE,
        c.singleLine ? IDC_LINEMODE_SINGLE : IDC_LINEMODE_BOTH);
    CheckDlgButton(hWnd, IDC_BOTHDIRS,
        c.bothDirs ? BST_CHECKED : BST_UNCHECKED);

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hWnd, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }

    if (g_hFont) { DeleteObject(g_hFont); g_hFont = nullptr; }
    return 0;
}

// ============================================================================
// Entry point
// ============================================================================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    SetProcessDPIAware();

    int    argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    enum Mode { M_CONFIG, M_RUN, M_TEST_MAIN, M_TEST_SECOND } mode = M_CONFIG;

    Config cli;
    bool colorSet  = false;
    bool modeSet   = false;
    bool schemaSet = false;
    bool bdirsSet  = false;
    bool speedSet  = false;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"/run") == 0 || _wcsicmp(argv[i], L"-run") == 0) {
            mode = M_RUN;
        } else if (_wcsicmp(argv[i], L"/test") == 0) {
            int which = 2;
            if (i + 1 < argc) { which = _wtoi(argv[++i]); if (which != 1) which = 2; }
            mode = (which == 1) ? M_TEST_MAIN : M_TEST_SECOND;
        } else if (_wcsicmp(argv[i], L"/color")    == 0 && i + 1 < argc) {
            cli.colorIndex  = _wtoi(argv[++i]); colorSet  = true;
        } else if (_wcsicmp(argv[i], L"/schema")   == 0 && i + 1 < argc) {
            cli.schemaIndex = _wtoi(argv[++i]); schemaSet = true;
        } else if (_wcsicmp(argv[i], L"/single")   == 0) {
            cli.singleLine = true;  modeSet  = true;
        } else if (_wcsicmp(argv[i], L"/both")     == 0) {
            cli.singleLine = false; modeSet  = true;
        } else if (_wcsicmp(argv[i], L"/bothdirs") == 0) {
            cli.bothDirs   = true;  bdirsSet = true;
        } else if (_wcsicmp(argv[i], L"/speed") == 0 && i + 1 < argc) {
            cli.sweepPeriodMs = _wtoi(argv[++i]); speedSet = true;
        }
    }
    if (argv) LocalFree(argv);

    // ---- Background run (called by Task Scheduler) ----
    if (mode == M_RUN) {
        HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\TVSaverLinesRunMutex_v2");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (mtx) CloseHandle(mtx);
            return 0;
        }
        Config c = LoadConfig();
        int rc = RunAnimation(false, c.runDurationSec * 1000, false, c);
        if (mtx) CloseHandle(mtx);
        return rc;
    }

    // ---- Test preview (spawned by the Run Test button) ----
    if (mode == M_TEST_MAIN || mode == M_TEST_SECOND) {
        Config c = LoadConfig();
        if (colorSet  && cli.colorIndex  >= 0 && cli.colorIndex  < kColorCount)
            c.colorIndex  = cli.colorIndex;
        if (schemaSet && cli.schemaIndex >= 0 && cli.schemaIndex < kSchemaCount)
            c.schemaIndex = cli.schemaIndex;
        if (modeSet)  c.singleLine    = cli.singleLine;
        if (bdirsSet) c.bothDirs      = cli.bothDirs;
        if (speedSet && cli.sweepPeriodMs >= 500 && cli.sweepPeriodMs <= 30000)
            c.sweepPeriodMs = cli.sweepPeriodMs;
        return RunAnimation(mode == M_TEST_MAIN, TEST_DURATION_MS, true, c);
    }

    // ---- No arguments → show configuration dialog ----
    return ShowConfigDialog(hInst);
}