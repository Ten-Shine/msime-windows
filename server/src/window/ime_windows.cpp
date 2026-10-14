#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "window/candidate_presenter.h"
#include "window/floating_toolbar_presenter.h"
#include "window/tray_menu_presenter.h"
#include "window/window_opacity_policy.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <fmt/xchar.h>
#include "webview2/windows_webview2.h"
#include "utils/window_utils.h"
#include <dwmapi.h>
#include "ipc/event_listener.h"
#include "window_hook.h"
#include "window/caret_state_indicator.h"
#include "window/caret_state_indicator_policy.h"
#include "log/candidate_diag_log.h"
#include "resource/resource.h"
#include "window/ime_windows_internal.h"

#pragma comment(lib, "dwmapi.lib")

using namespace ime_windows_detail;

namespace ime_windows_detail
{
bool g_is_ime_active = false;
// Drop stale FineTune measure callbacks when a newer show/update supersedes them.
std::atomic<uint64_t> g_candidate_finetune_generation{0};
std::atomic<bool> g_candidate_layout_inflight{false};
// Drop clip measurements that completed after newer candidate content was
// already submitted to WebView2.
std::atomic<uint64_t> g_candidate_content_generation{0};
int g_last_placed_caret_x = Global::INVALID_Y;
int g_last_placed_caret_y = Global::INVALID_Y;
bool g_candidate_session_anchor_valid = false;
POINT g_candidate_session_anchor{};
bool g_has_last_candidate_clip = false;
std::pair<double, double> g_last_candidate_clip_size{};
FLOAT g_last_candidate_clip_scale = 1.0f;
bool g_candidate_placed_above_caret = false;
std::pair<double, double> g_last_candidate_card_size{};
bool g_has_candidate_clip_envelope = false;
double g_clip_envelope_top_dip = 0.0;
double g_clip_envelope_bottom_dip = 0.0;
double g_clip_envelope_left_dip = 0.0;
double g_clip_envelope_right_dip = 0.0;
// SetWindowPos on the candidate host can synchronously deliver WM_DPICHANGED on
// the same call stack (hide-to-offscreen, FineTune cross-monitor moves). Applying
// the suggested rect or re-entering FineTune from that handler races the hide
// park and can recurse until the UI thread stalls — seen when switching apps.
int g_candidate_dpi_change_suppress_count = 0;
// Last frame actually painted, used to drop duplicate shows. Cleared on hide so
// the next session always paints.
std::wstring g_last_rendered_candidate_signature;
ULONGLONG g_last_rendered_candidate_tick = 0;
// 候选窗渐变状态（不透明度调节 / 隐藏淡出）。All on the UI thread, like the
// applies; the hide fade keeps ::is_global_wnd_cand_shown true until it lands,
// so a show inside the fade cancels it instead of racing the teardown.
CandidateFadeKind g_candidate_fade_kind = CandidateFadeKind::None;
ULONGLONG g_candidate_fade_start = 0;
BYTE g_candidate_fade_from = 255;
BYTE g_candidate_fade_target = 255;
BYTE g_candidate_current_alpha = 255;

// WM_SETTINGCHANGE lParam points to "ImmersiveColorSet" when the user flips
// the Windows app light/dark theme. Only meaningful for the D2D presenters:
// they own their colors, while WebView2 pages restyle themselves.
bool IsSystemLightDarkToggle(UINT message, LPARAM lParam)
{
    return message == WM_SETTINGCHANGE && lParam != 0 && UseD2dSmallWindowUi() &&
           lstrcmpiW(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0;
}

void SyncHostWebViewBounds(ICoreWebView2Controller *controller, HWND hwnd)
{
    if (!controller || !hwnd)
    {
        return;
    }
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    controller->put_Bounds(bounds);
    controller->NotifyParentWindowPositionChanged();
}

// WebView2 needs a real on-monitor, "visible" HWND to finish controller/raster
// setup. Cloak keeps that warmup invisible (same idea as the settings window).
void SetHostWindowCloaked(HWND hwnd, bool cloaked)
{
    if (!hwnd)
    {
        return;
    }
    BOOL value = cloaked ? TRUE : FALSE;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
    if (hwnd == ::global_hwnd)
    {
        CAND_DIAG_LOGF(L"candidate-frame cloak requested={} hr={:#x} actual={} tick={}", cloaked,
                       static_cast<unsigned>(hr), IsHostWindowCloaked(hwnd), GetTickCount64());
    }
}

// A cloaked host is still "visible" to IsWindowVisible and still reports its
// rect to window enumeration, so the trace has to distinguish the two.
bool IsHostWindowCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    if (!hwnd || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
    {
        return false;
    }
    return cloaked != 0;
}
} // namespace ime_windows_detail

namespace
{
// Who cloaked it matters: the app value is ours, while shell/inherited means
// something outside this process (virtual desktop switch, shell policy) took
// the toolbar off screen and no amount of ShowWindow will bring it back.
std::wstring DescribeCloak(DWORD cloaked)
{
    if (cloaked == 0)
    {
        return L"no";
    }
    std::wstring description;
    if (cloaked & DWM_CLOAKED_APP)
    {
        description += L"app,";
    }
    if (cloaked & DWM_CLOAKED_SHELL)
    {
        description += L"shell,";
    }
    if (cloaked & DWM_CLOAKED_INHERITED)
    {
        description += L"inherited,";
    }
    if (description.empty())
    {
        return L"yes(" + std::to_wstring(cloaked) + L")";
    }
    description.back() = L')';
    return L"yes(" + description;
}

// A WebView2 host can be fully transparent while every ordinary check says it
// is fine, which is what "invisible but the screenshot tool can still frame it"
// and "invisible but the clicks land on the right item" both mean. Only a
// handful of things produce that, and none of them are visible from
// IsWindowVisible: a layered window whose alpha was lost or whose WS_EX_LAYERED
// style went away paints nothing at all, a host still DWM-cloaked is excluded
// from composition while input routing continues normally, a controller that
// believes it is hidden or was given empty bounds paints nothing either, and a
// host parked off every monitor has nowhere to paint. Record all of them
// together so a blank window can be attributed instead of guessed at.
std::wstring DescribeHostWindowState(HWND hwnd, bool has_controller, bool webview_visible, const RECT &webview_bounds)
{
    if (!hwnd)
    {
        return L"host=none";
    }

    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);
    RECT client_rect{};
    GetClientRect(hwnd, &client_rect);

    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    COLORREF color_key = 0;
    BYTE alpha = 0;
    DWORD layered_flags = 0;
    const bool layered_ok = GetLayeredWindowAttributes(hwnd, &color_key, &alpha, &layered_flags) != FALSE;
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    // WebView2 hosts its renderer in a child HWND. If that child is missing or
    // zero-sized the host is an empty shell no matter how healthy it looks.
    const HWND child = FindWindowExW(hwnd, nullptr, nullptr, nullptr);
    wchar_t child_class[64] = {};
    RECT child_rect{};
    if (child)
    {
        GetClassNameW(child, child_class, ARRAYSIZE(child_class));
        GetWindowRect(child, &child_rect);
    }

    return fmt::format(L"rect=({},{},{}x{}) client={}x{} on_monitor={} visible={} cloaked={} "
                       L"layered_attrs={} alpha={} lwa_flags={:#x} ex_layered={} ex_topmost={} "
                       L"controller={} wv_visible={} wv_bounds={}x{} child={} child_visible={} child_size={}x{}",
                       window_rect.left, window_rect.top, window_rect.right - window_rect.left,
                       window_rect.bottom - window_rect.top, client_rect.right, client_rect.bottom,
                       MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr, IsWindowVisible(hwnd) != FALSE,
                       DescribeCloak(cloaked), layered_ok ? L"ok" : L"MISSING", static_cast<unsigned>(alpha),
                       layered_flags, (ex_style & WS_EX_LAYERED) != 0, (ex_style & WS_EX_TOPMOST) != 0, has_controller,
                       webview_visible, webview_bounds.right - webview_bounds.left,
                       webview_bounds.bottom - webview_bounds.top, child ? child_class : L"NONE",
                       child && IsWindowVisible(child) != FALSE, child_rect.right - child_rect.left,
                       child_rect.bottom - child_rect.top);
}

} // namespace

namespace ime_windows_detail
{
std::wstring DescribeFloatingToolbarHostState()
{
    bool webview_visible = false;
    RECT webview_bounds{};
    const bool has_controller = GetFloatingToolbarWebviewState(webview_visible, webview_bounds);
    return DescribeHostWindowState(::global_hwnd_ftb, has_controller, webview_visible, webview_bounds);
}
} // namespace ime_windows_detail

void SetCandidateHostCloaked(bool cloaked)
{
    SetHostWindowCloaked(::global_hwnd, cloaked);
}

namespace
{

BYTE CandidateWindowBaseAlpha()
{
    // D2D 端前景/背景不透明度落在绘制颜色里（CandidatePresenter::ApplySkin），窗口级
    // alpha 只服务隐藏淡出，基准是"不压"。这里若返回背景不透明度，整帧——候选文字
    // 也在里面——都会乘上它，把背景调到 1% 就等于把前景也调到 1%。
    // WebView2 端没有前景/背景之分：整窗一个 alpha，取"背景不透明度"作为窗口的
    // 通透程度（前景不透明度在 WebView2 下无效）。
    return FanyImeUi::WindowOpacityBaseAlpha(UseD2dSmallWindowUi(), GetConfiguredCandidateWindowBackgroundOpacity());
}

// 两种后端各一条写入路径：WebView2 宿主持分层窗口，直接改 LWA_ALPHA；D2D 端
// 没有重定向位图，整帧不透明度由 presenter 用一层 D2D 图层画出来。
// 两条路径都只写"整帧"alpha，调用方要么在跑渐变，要么在把 alpha 放回
// CandidateWindowBaseAlpha()——不要在这里传前景/背景不透明度。
// g_candidate_current_alpha 记录最后一次写入的值，渐变从它起步。
void SetCandidateHostAlpha(BYTE alpha)
{
    g_candidate_current_alpha = alpha;
    if (UseD2dSmallWindowUi())
    {
        CandidatePresenter::Instance().SetFrameOpacity(static_cast<float>(alpha) / 255.0f);
        return;
    }
    SetLayeredWindowAttributes(::global_hwnd, 0, alpha, LWA_ALPHA);
}

} // namespace

namespace ime_windows_detail
{

void BeginCandidateFade(BYTE target, CandidateFadeKind kind)
{
    if (!::global_hwnd)
    {
        return;
    }
    // 只有同一种渐变允许在路上改目标（连续调节不透明度时从当前值重新起步）；
    // 隐藏淡出进行中再调不透明度没有意义——窗口正要消失，新值由收尾放回。
    if (g_candidate_fade_kind != CandidateFadeKind::None && g_candidate_fade_kind != kind)
    {
        return;
    }
    g_candidate_fade_kind = kind;
    g_candidate_fade_start = GetTickCount64();
    g_candidate_fade_from = g_candidate_current_alpha;
    g_candidate_fade_target = target;
    SetTimer(::global_hwnd, TIMER_ID_CANDIDATE_HIDE_FADE, kFloatingToolbarAutoHideFadeStepMs, nullptr);
}

void CancelCandidateFade()
{
    if (g_candidate_fade_kind == CandidateFadeKind::None)
    {
        return;
    }
    if (::global_hwnd)
    {
        KillTimer(::global_hwnd, TIMER_ID_CANDIDATE_HIDE_FADE);
    }
    g_candidate_fade_kind = CandidateFadeKind::None;
    SetCandidateHostAlpha(CandidateWindowBaseAlpha());
}

void StepCandidateFade(HWND hwnd)
{
    if (g_candidate_fade_kind == CandidateFadeKind::None)
    {
        KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_FADE);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - g_candidate_fade_start;
    const UINT fadeMs = g_candidate_fade_kind == CandidateFadeKind::Hide ? kCandidateHideFadeMs : kWindowOpacityFadeMs;
    if (elapsed < fadeMs)
    {
        SetCandidateHostAlpha(
            FanyImeUi::WindowFadeAlpha(g_candidate_fade_from, g_candidate_fade_target, elapsed, fadeMs));
        return;
    }
    KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_FADE);
    if (g_candidate_fade_kind == CandidateFadeKind::Hide)
    {
        g_candidate_fade_kind = CandidateFadeKind::None;
        SetCandidateHostAlpha(0);
        ApplyCandidateHideNow(hwnd);
        return;
    }
    // 不透明度渐变：落在新的基准值上即结束。
    g_candidate_fade_kind = CandidateFadeKind::None;
    SetCandidateHostAlpha(g_candidate_fade_target);
}

void ApplyCandidateHideNow(HWND hwnd)
{
    // 渐变中的话到这儿结束：宿主即将被 cloak，alpha 放回基准值——不放回的
    // 话下一次 show 会从 0（看不见）开始。
    if (g_candidate_fade_kind != CandidateFadeKind::None)
    {
        KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_FADE);
        g_candidate_fade_kind = CandidateFadeKind::None;
    }
    ::is_global_wnd_cand_shown = false;
    Global::SetCandidateWindowRenderedVisible(false);
    g_last_rendered_candidate_signature.clear();
    KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
    g_candidate_session_anchor_valid = false;
    ++g_candidate_content_generation;
    g_candidate_layout_inflight.store(false);
    ++g_candidate_finetune_generation;
    Global::MarginTop = 0;
    Global::MarginLeft = 0;
    g_has_last_candidate_clip = false;
    ResetCandidatePlacementMemory();
    {
        SuppressCandidateDpiChange suppressDpi;
        if (CandidatePresenter::Instance().IsBound())
        {
            CandidatePresenter::Instance().Hide();
        }
        else if (!webviewControllerCandWnd)
        {
            // Moving this HWND while CreateCoreWebView2Controller is in
            // flight makes the candidate controller fail (menu/FTB still work).
            SetHostWindowCloaked(hwnd, true);
        }
        else
        {
            ClearCandidateWindowRegion(hwnd);
            // Park off-screen but keep the stable quarter-screen host size.
            // Shrinking back to the default card size made the next show race
            // put_Bounds / margins against a tiny HWND and park the card at
            // the monitor's left edge.
            const auto parkHost = ComputeQuarterScreenHostPixels(GetMainMonitorCoordinates());
            SetWindowPos(hwnd, HWND_TOP, 0, Global::INVALID_Y, parkHost.first, parkHost.second, SWP_SHOWWINDOW);
            SetHostWindowCloaked(hwnd, true);
            UpdateHtmlContentWithJavaScript(webviewCandWnd, L"");
        }
    }
    // 收尾后放回基准不透明度：宿主已 cloak 或移出屏幕，这一步不可见。
    SetCandidateHostAlpha(CandidateWindowBaseAlpha());
}

} // namespace ime_windows_detail

void ApplyConfiguredCandidateWindowOpacity()
{
    if (!::global_hwnd)
    {
        return;
    }
    if (UseD2dSmallWindowUi())
    {
        // D2D：前景/背景不透明度落在绘制颜色里（CandidatePresenter::ApplySkin），
        // 重新套一遍皮肤即生效——与字号、字体这些外观项一致，没有渐变：D2D 宿主
        // 没有窗口级 alpha 可用，整帧淡出只留给隐藏。
        CandidatePresenter::Instance().ApplySkin();
        if (::is_global_wnd_cand_shown)
        {
            CandidatePresenter::Instance().Present();
        }
        return;
    }
    // 没上过屏（启动、驻留、暖机）直接落值：淡了也看不见，还会把启动路径
    // 拖慢。上着屏才从当前值渐变过去。
    if (!::is_global_wnd_cand_shown || !IsCandidateHostPaintedVisible(::global_hwnd))
    {
        if (g_candidate_fade_kind != CandidateFadeKind::None)
        {
            KillTimer(::global_hwnd, TIMER_ID_CANDIDATE_HIDE_FADE);
            g_candidate_fade_kind = CandidateFadeKind::None;
        }
        SetCandidateHostAlpha(CandidateWindowBaseAlpha());
        return;
    }
    BeginCandidateFade(CandidateWindowBaseAlpha(), CandidateFadeKind::Opacity);
}

std::wstring DescribeCandidateHostState()
{
    bool webview_visible = false;
    RECT webview_bounds{};
    const bool has_controller = GetCandidateWebviewState(webview_visible, webview_bounds);
    return fmt::format(L"logical_shown={} nav_ready={} {}", ::is_global_wnd_cand_shown, IsCandidateWebviewReady(),
                       DescribeHostWindowState(::global_hwnd, has_controller, webview_visible, webview_bounds));
}

// External linkage, unlike its toolbar counterpart: the menu's own lifecycle
// events live in windows_webview2_menu.cpp and are the ones worth recording.
std::wstring DescribeTrayMenuHostState()
{
    bool webview_visible = false;
    RECT webview_bounds{};
    const bool has_controller = GetTrayMenuWebviewState(webview_visible, webview_bounds);
    return DescribeHostWindowState(::global_hwnd_menu, has_controller, webview_visible, webview_bounds);
}

namespace
{

void PrepareLayeredHostWindow(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    MARGINS mar = {-1};
    DwmExtendFrameIntoClientArea(hwnd, &mar);
}

// Show on a real monitor while cloaked so WebView2 can warm up without a flash.
void WarmupHostWindowCloaked(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    SetHostWindowCloaked(hwnd, true);
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
}
} // namespace

void ApplyConfiguredInputScheme()
{
    FanyNamedPipe::EnqueueReloadInputSessionTask();
    UpdateFtbInputModeState(::webviewFtbWnd, GetActiveInputMode() == "japanese" ? 1 : 0);
    BroadcastConfiguredInputModeState();
}

void ApplyConfiguredShuangpinSchema()
{
    FanyNamedPipe::EnqueueReloadInputSessionTask();
    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged,
                                           IsConfiguredShuangpinSemicolonFinal() ? L"1" : L"0");
}

LRESULT RegisterCandidateWindowMessage()
{

    WM_SHOW_MAIN_WINDOW = RegisterWindowMessage(L"WM_SHOW_MAIN_WINDOW");
    WM_HIDE_MAIN_WINDOW = RegisterWindowMessage(L"WM_HIDE_MAIN_WINDOW");
    WM_MOVE_CANDIDATE_WINDOW = RegisterWindowMessage(L"WM_MOVE_CANDIDATE_WINDOW");
    return 0;
}

LRESULT RegisterIMEWindowsClass(WNDCLASSEX &wcex, HINSTANCE hInstance)
{
    //
    // 注册窗口类
    //
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_IME_ICON));
    wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_IME_ICON));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* We do not need background color, otherwise it will flash when rendering */
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    return 0;
}

int CreateCandidateWindow(HINSTANCE hInstance)
{
    //
    // 候选框窗口
    // Create on a real monitor and show while DWM-cloaked. Hidden / far
    // off-screen hosts prevent WebView2 from finishing init; cloaking avoids
    // the old (100,100)/(200,200) startup flash without breaking warmup.
    //
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
    const bool d2dUi = UseD2dSmallWindowUi();
    DWORD dwExStyle = d2dUi ? (WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
                            : (WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    FLOAT scale = GetForegroundWindowScale();
    const auto quarterHost = ComputeQuarterScreenHostPixels(GetMainMonitorCoordinates());
    const int candWidth = d2dUi ? (std::max)(8, static_cast<int>(32 * scale)) : quarterHost.first;
    const int candHeight = d2dUi ? (std::max)(8, static_cast<int>(32 * scale)) : quarterHost.second;

    HWND hwnd_cand = CreateWindowEx( //
        dwExStyle,                   //
        szWindowClass,               //
        lpWindowNameCand,            //
        WS_POPUP,                    //
        100,                         //
        100,                         //
        candWidth,                   //
        candHeight,                  //
        nullptr,                     //
        nullptr,                     //
        hInstance,                   //
        nullptr                      //
    );                               //

    if (!hwnd_cand)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    else
    {
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(hwnd_cand, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions,
                              sizeof(disableTransitions));
    }

    ::global_hwnd = hwnd_cand;
    if (d2dUi)
    {
        CandidatePresenter::Instance().Bind(hwnd_cand);
    }
    else
    {
        PrepareLayeredHostWindow(hwnd_cand);
    }
    // PrepareLayeredHostWindow leaves the host fully opaque; apply the
    // configured opacity before the first show either way.
    ApplyConfiguredCandidateWindowOpacity();
    CAND_DIAG_LOGF(L"candidate host created backend={} {}", d2dUi ? L"d2d" : L"webview2", DescribeCandidateHostState());

    // A client can activate while the pipe server is already listening but this
    // window does not exist yet, which is the race that leaves the toolbar
    // hidden after a server restart. The queued message waits for the loop that
    // starts once the remaining windows are up.
    FanyNamedPipe::ReplayDeferredClientActivation();

    //
    // 任务栏托盘区的菜单窗口
    // TODO: 这里的初始 width 和 height 需要设置足够大，不然，底部的 item 会不接受响应。不然，也可以在 wndProc
    // 中刷新一下 webview
    //
    dwExStyle = WS_EX_LAYERED |                               //
                WS_EX_TOOLWINDOW |                            //
                WS_EX_NOACTIVATE;                             //
                                                              // WS_EX_TOPMOST;              //
    HWND hwnd_menu = CreateWindowEx(                          //
        dwExStyle,                                            //
        szWindowClass,                                        //
        lpWindowNameMenu,                                     //
        WS_POPUP,                                             //
        200,                                                  //
        200,                                                  //
        static_cast<int>((::MENU_WINDOW_WIDTH)*scale),        //
        static_cast<int>((::MENU_WINDOW_HEIGHT * 2) * scale), //
        nullptr,                                              //
        nullptr,                                              //
        hInstance,                                            //
        nullptr                                               //
    );                                                        //
    if (!hwnd_menu)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    PrepareLayeredHostWindow(hwnd_menu);
    ::global_hwnd_menu = hwnd_menu;
    if (UseD2dSmallWindowUi())
    {
        TrayMenuPresenter::Instance().Bind(hwnd_menu);
    }

    //
    // floating toolbar 窗口
    //
    ::FTB_WND_WIDTH = ConfiguredFloatingToolbarWidth();
    ::FTB_WND_HEIGHT = ConfiguredFloatingToolbarHeight();
    dwExStyle = WS_EX_LAYERED |    //
                WS_EX_TOOLWINDOW | //
                WS_EX_NOACTIVATE;  //
                                   // WS_EX_TOPMOST;                               //
    MonitorCoordinates ftbMonitor = GetMainMonitorCoordinates();
    const int ftbShadowWidth = GetConfiguredFloatingToolbarShadow() ? ::FTB_WND_SHADOW_WIDTH : 0;
    const int ftbWidth = static_cast<int>((::FTB_WND_WIDTH + ftbShadowWidth) * scale);
    const int ftbHeight = static_cast<int>((::FTB_WND_HEIGHT + ftbShadowWidth) * scale);
    const int ftbTaskbarHeight = GetTaskbarHeight();
    const int ftbCornerInset = static_cast<int>(std::lround(10.0 * static_cast<double>(scale > 0 ? scale : 1.0f)));
    const int ftbX = ftbMonitor.right - ftbWidth - ftbCornerInset;
    const int ftbY = ftbMonitor.bottom - ftbHeight - ftbTaskbarHeight - ftbCornerInset;
    // The caret badge is optional: without its window every post is a no-op,
    // so a creation failure must not take the input windows down with it.
    HWND hwnd_caret_state =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, szWindowClass,
                        lpWindowNameCaretState, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (hwnd_caret_state)
    {
        ::global_hwnd_caret_state = hwnd_caret_state;
        SetLayeredWindowAttributes(hwnd_caret_state, 0, 245, LWA_ALPHA);
        ShowWindow(hwnd_caret_state, SW_HIDE);
    }

    HWND hwnd_ftb = CreateWindowEx( //
        dwExStyle,                  //
        szWindowClass,              //
        lpWindowNameFtb,            //
        WS_POPUP,                   //
        ftbX,                       //
        ftbY,                       //
        ftbWidth,                   //
        ftbHeight,                  //
        nullptr,                    //
        nullptr,                    //
        hInstance,                  //
        nullptr                     //
    );                              //
    if (!hwnd_ftb)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
    PrepareLayeredHostWindow(hwnd_ftb);
    ::global_hwnd_ftb = hwnd_ftb;
    FanyNamedPipe::RegisterStatusSnapshotWindow(hwnd_ftb);
    if (UseD2dSmallWindowUi())
    {
        FloatingToolbarPresenter::Instance().Bind(hwnd_ftb);
    }
    // PrepareLayeredHostWindow leaves the host fully opaque; apply the
    // configured opacity before the first show either way.
    ApplyConfiguredFloatingToolbarOpacity();

    // Cloaked show: WebView2 sees a visible on-monitor host; the user does not.
    WarmupHostWindowCloaked(hwnd_cand);
    WarmupHostWindowCloaked(hwnd_menu);
    // The toolbar used to be the one small window left out of this, so whenever
    // no client was active at startup its controller was created against a
    // hidden host. WebView2 then never finished raster setup, and the later
    // ShowWindow produced a layered window that reports a correct rect to window
    // enumeration while painting nothing at all.
    WarmupHostWindowCloaked(hwnd_ftb);
    ApplyConfiguredFloatingToolbarVisibility(L"startup");
    UpdateWindow(hwnd_ftb);

    //
    // Preparing webview2 env (skipped when appearance.ui_backend is d2d)
    //
    if (!UseD2dSmallWindowUi())
    {
        PrepareHtmlForWnds();
        /* 候选框、托盘语言区右键菜单和 floating toolbar 共用一个 WebView2 environment */
        InitSmallWindowWebviews(hwnd_cand, hwnd_menu, hwnd_ftb);

        /* 菜单窗口：首屏导航完成后量一次尺寸（暂不 TOPMOST） */
        SetTimer(hwnd_menu, TIMER_ID_INIT_WEBVIEW_MENU, 200, nullptr);
    }

    /* 监听文本配置文件变化，并同步到运行中的候选框。Settings 已是独立进程。 */
    SetTimer(hwnd_cand, TIMER_ID_CONFIG_SYNC, 300, nullptr);

    /* floating toolbar：再确认一次落在主屏右下角（不依赖 WebView 就绪） */
    SetTimer(hwnd_ftb, TIMER_ID_MOVE_WEBVIEW_FTB, 200, nullptr);

    //
    // 注册一下全局钩子
    //
    InitServerCapsLockState();
    g_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!g_hHook)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return 1;
    }
#ifdef FANY_DEBUG
    (void)0;
#endif

    // The hook lives for the whole process; nothing ever unhooks it, so the handle is not kept.
    SetWinEventHook(                 //
        EVENT_SYSTEM_FOREGROUND,     //
        EVENT_OBJECT_LOCATIONCHANGE, //
        nullptr,                     //
        WinEventProc,                //
        0,                           //
        0,                           //
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* 卸载钩子 */
    UnhookWindowsHookEx(g_hHook);

    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_SHOWWINDOW)
    {
        UpdateSmallWindowWebviewVisibility(hwnd, wParam != FALSE);
        if (hwnd == ::global_hwnd)
        {
            CAND_DIAG_LOGF(L"host WM_SHOWWINDOW showing={} reason={} {}", wParam != FALSE,
                           static_cast<unsigned long long>(lParam), DescribeCandidateHostState());
        }
    }

    if (message == WM_APPLY_IME_CONFIG || message == WM_APPLY_IME_INPUT_SCHEME)
    {
        if (hwnd != ::global_hwnd && ::global_hwnd && IsWindow(::global_hwnd))
        {
            PostMessageW(::global_hwnd, message, wParam, lParam);
            return 0;
        }
    }

    /* 候选窗口 */
    if (hwnd == ::global_hwnd)
    {
        return WndProcCandWindow(hwnd, message, wParam, lParam);
    }

    /* tray icon 菜单窗口 */
    if (hwnd == ::global_hwnd_menu)
    {
        return WndProcMenuWindow(hwnd, message, wParam, lParam);
    }

    /* floating toolbar 窗口 */
    if (hwnd == ::global_hwnd_ftb)
    {
        return WndProcFtbWindow(hwnd, message, wParam, lParam);
    }
    if (hwnd == ::global_hwnd_caret_state)
    {
        return WndProcCaretStateWindow(hwnd, message, wParam, lParam);
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

namespace
{
void DiscardPendingCaretStateRequests(HWND hwnd)
{
    MSG pending{};
    while (PeekMessageW(&pending, hwnd, WM_SHOW_CARET_STATE, WM_SHOW_CARET_STATE, PM_REMOVE))
        delete reinterpret_cast<CaretStateIndicator::ShowRequest *>(pending.lParam);
    while (PeekMessageW(&pending, hwnd, WM_MOVE_CARET_STATE, WM_MOVE_CARET_STATE, PM_REMOVE))
        delete reinterpret_cast<POINT *>(pending.lParam);
}
} // namespace

LRESULT CALLBACK WndProcCaretStateWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (IsSystemLightDarkToggle(message, lParam))
    {
        // The palette follows the next Show; a stale badge simply goes away.
        CaretStateIndicator::Hide(hwnd);
        ::is_global_wnd_caret_state_shown = false;
        return 0;
    }

    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        CaretStateIndicator::Paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (CaretStateIndicator::HandleTimer(hwnd, wParam))
        {
            ::is_global_wnd_caret_state_shown = false;
            return 0;
        }
        break;
    case WM_SHOW_CARET_STATE: {
        std::unique_ptr<CaretStateIndicator::ShowRequest> request(
            reinterpret_cast<CaretStateIndicator::ShowRequest *>(lParam));
        if (!request ||
            !FanyImeUi::ShouldShowCaretStateIndicator(GetConfiguredCaretStateIndicatorEnabled(), g_is_ime_active,
                                                      request->uiLess, request->caret.x, request->caret.y) ||
            IsForegroundD3DExclusiveFullscreen())
        {
            CaretStateIndicator::Hide(hwnd);
            ::is_global_wnd_caret_state_shown = false;
            return 0;
        }
        const bool topmost = EnsureSmallWindowsTopmost(L"show-caret-state");
        ::is_global_wnd_caret_state_shown = CaretStateIndicator::Show(hwnd, request->badge, request->caret, topmost);
        return 0;
    }
    case WM_MOVE_CARET_STATE: {
        std::unique_ptr<POINT> caret(reinterpret_cast<POINT *>(lParam));
        if (::is_global_wnd_caret_state_shown &&
            (!caret || !FanyImeUi::IsUsableCaretAnchor(caret->x, caret->y) ||
             !CaretStateIndicator::Reposition(hwnd, *caret, EnsureSmallWindowsTopmost(L"move-caret-state"))))
        {
            CaretStateIndicator::Hide(hwnd);
            ::is_global_wnd_caret_state_shown = false;
        }
        return 0;
    }
    case WM_HIDE_CARET_STATE:
        CaretStateIndicator::Hide(hwnd);
        // Posted messages are FIFO. Do not discard shows queued after this
        // hide: they belong to a newer state switch on the same UI thread.
        ::is_global_wnd_caret_state_shown = false;
        return 0;
    case WM_NCDESTROY:
        ::global_hwnd_caret_state = nullptr;
        DiscardPendingCaretStateRequests(hwnd);
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}
