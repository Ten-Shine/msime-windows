// 悬浮工具栏宿主：按配置计算尺寸、拖动后收回可见屏幕、随 DPI 重新布局与放置、
// 显隐决策与自动隐藏，以及 WndProcFtbWindow。
#include "window/ime_windows_internal.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "window/floating_toolbar_presenter.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <fmt/xchar.h>
#include "utils/window_utils.h"
#include "window/floating_toolbar_visibility_policy.h"
#include "window/window_opacity_policy.h"
#include "log/ftb_diag_log.h"
#include <windowsx.h>

using namespace ime_windows_detail;

namespace ime_windows_detail
{
int ConfiguredFloatingToolbarWidth()
{
    const FloatingToolbarItemsConfig &items = GetConfiguredFloatingToolbarItems();
    const int optional_item_count = static_cast<int>(items.fullwidth) + static_cast<int>(items.punctuation) +
                                    static_cast<int>(items.character_set) + static_cast<int>(items.emoji) +
                                    static_cast<int>(items.screen_keyboard) + static_cast<int>(items.settings);
    // 59 DIPs covers the drag handle, divider, borders, mandatory CN/EN button and
    // container padding. Each optional button adds 24 DIPs plus the 6-DIP gap.
    // User scale/font_size multiply the design size independently of system DPI.
    const double userScale = GetConfiguredFloatingToolbarScale();
    const double fontFactor = static_cast<double>(GetConfiguredFloatingToolbarFontSize()) / 24.0;
    return static_cast<int>(std::lround((59 + optional_item_count * 30) * userScale * fontFactor));
}

int ConfiguredFloatingToolbarHeight()
{
    const double userScale = GetConfiguredFloatingToolbarScale();
    const double fontFactor = static_cast<double>(GetConfiguredFloatingToolbarFontSize()) / 24.0;
    return static_cast<int>(std::lround(35.0 * userScale * fontFactor));
}

bool FloatingToolbarItemsEqual(const FloatingToolbarItemsConfig &left, const FloatingToolbarItemsConfig &right)
{
    return left.fullwidth == right.fullwidth && left.punctuation == right.punctuation &&
           left.character_set == right.character_set && left.emoji == right.emoji &&
           left.screen_keyboard == right.screen_keyboard && left.settings == right.settings;
}
} // namespace ime_windows_detail

namespace
{
// Transparent shadow margin that may hang off a screen edge, so the visible bar
// can sit flush with it like the Windows language bar. ClipWindowToVisibleScreens
// then keeps that overhang from drawing or swallowing clicks over the taskbar.
RECT FloatingToolbarShadowInsetsPx(int hostWidthPx, int hostHeightPx, FLOAT scale)
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        return FloatingToolbarPresenter::Instance().ShadowInsetsPx(hostWidthPx, hostHeightPx, scale);
    }
    return {};
}

// 皮肤图片模式的工具栏是异形窗口：裁到屏幕内的同时要保住图片的形状，不能拿矩形把它盖掉。
void ClipFloatingToolbarToVisibleScreens(HWND hwnd)
{
    ClipWindowToVisibleScreens(hwnd, ScreenArea::WorkArea, FloatingToolbarPresenter::Instance().ShapeRegion());
}

void KeepFloatingToolbarInsideVisibleScreens(HWND hwnd)
{
    RECT rect{};
    if (!hwnd || !GetWindowRect(hwnd, &rect))
    {
        return;
    }

    // Pull the toolbar back on the monitor nearest to its center. Picking the
    // monitor with MonitorFromRect's largest intersection made a toolbar that
    // straddles a seam (mixed-DPI setups, or a secondary screen taller than the
    // primary) snap back to the screen it came from.
    const RECT before = rect;
    HMONITOR monitor = nullptr;
    if (!KeepRectOnVisibleScreens(
            rect, ScreenArea::WorkArea, &monitor,
            FloatingToolbarShadowInsetsPx(rect.right - rect.left, rect.bottom - rect.top, GetWindowScale(hwnd))))
    {
        ClipFloatingToolbarToVisibleScreens(hwnd);
        return;
    }

    SetWindowPos(hwnd, nullptr, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ClipFloatingToolbarToVisibleScreens(hwnd);

    MONITORINFO info{sizeof(info)};
    GetMonitorInfo(monitor, &info);
    FTB_DIAG_LOGF(L"ftb drag clamped from ({},{}) to ({},{}) work=({},{})-({},{})", before.left, before.top, rect.left,
                  rect.top, info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom);
}

// Recompute FTB outer HWND from design DIPs * current DPI. Placement used to be
// SWP_NOSIZE-only, so a live display-scale change left the host stuck at the
// create-time physical size while the content grew.
//
// reset_to_default_corner=true snaps to the primary-monitor bottom-right slot
// (startup / first layout). false keeps the current top-left so IME activate,
// fullscreen exit, and config toggles do not undo a user drag.
//
// scaleOverride>0: use that factor (WM_DPICHANGED's wParam) instead of
// GetWindowScale, which can briefly lag the message's new DPI.
//
// suggestedRect: WM_DPICHANGED's recommended placement (lParam). Windows
// computes it for the new DPI, keeping the toolbar under the cursor during a
// caption drag without flipping back and forth across the seam.
void LayoutFloatingToolbar(HWND hwnd, bool reset_to_default_corner, FLOAT scaleOverride = 0.0f,
                           const RECT *suggestedRect = nullptr)
{
    if (!hwnd)
    {
        return;
    }
    if (::FTB_CONTENT_WIDTH_DIP > 1.0 && ::FTB_CONTENT_HEIGHT_DIP > 1.0)
    {
        ::FTB_WND_WIDTH = static_cast<int>(std::ceil(::FTB_CONTENT_WIDTH_DIP));
        ::FTB_WND_HEIGHT = static_cast<int>(std::ceil(::FTB_CONTENT_HEIGHT_DIP));
    }
    else
    {
        ::FTB_WND_WIDTH = ConfiguredFloatingToolbarWidth();
        ::FTB_WND_HEIGHT = ConfiguredFloatingToolbarHeight();
    }
    FLOAT scale = scaleOverride > 0.0f ? scaleOverride : GetWindowScale(hwnd);
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    // Half-screen cap so content layout and HWND agree before show.
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForHwnd(hwnd);
    if (scaleOverride > 0.0f)
    {
        if (suggestedRect)
        {
            // The host is about to land on the suggested rect's monitor.
            limits.monitor = QueryHalfScreenDipLimitsForPoint({suggestedRect->left, suggestedRect->top}).monitor;
        }
        const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
        const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
        limits.scale = scale;
        limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(scale);
        limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    }
    ::FTB_WND_WIDTH =
        static_cast<int>(std::ceil(ClampWidthDipToHalfScreen(static_cast<double>(::FTB_WND_WIDTH), limits)));
    ::FTB_WND_HEIGHT =
        static_cast<int>(std::ceil(ClampHeightDipToHalfScreen(static_cast<double>(::FTB_WND_HEIGHT), limits)));
    // The presenter measures its shadow margin into the scene.
    const int width = static_cast<int>(std::ceil(::FTB_WND_WIDTH * static_cast<double>(scale)));
    const int height = static_cast<int>(std::ceil(::FTB_WND_HEIGHT * static_cast<double>(scale)));
    const int cornerInset = static_cast<int>(std::lround(10.0 * static_cast<double>(scale)));
    const RECT shadowInsets = FloatingToolbarShadowInsetsPx(width, height, scale);
    int posX = 0;
    int posY = 0;
    if (reset_to_default_corner)
    {
        // The inset is measured from the visible bar, not the shadow margin.
        MonitorCoordinates coordinates = GetMainMonitorCoordinates();
        const int taskbarHeight = GetTaskbarHeight();
        posX = coordinates.right - width + shadowInsets.right - cornerInset;
        posY = coordinates.bottom - height + shadowInsets.bottom - taskbarHeight - cornerInset;
    }
    else
    {
        // When optional buttons are added, the toolbar grows to the right from
        // the existing top-left; keep the resized host reachable without
        // pinning it to the monitor it currently sits on.
        // Toggling the shadow changes the margin, not the bar: the previous
        // insets keep the bar itself in place and inside the work area.
        const POINT pos = PlaceResizedHost(hwnd, width, height, suggestedRect, shadowInsets,
                                           ::FTB_HOST_INSETS_VALID ? &::FTB_HOST_INSETS_PX : nullptr);
        posX = pos.x;
        posY = pos.y;
    }
    // Never touch Z-order here: HWND_TOP would cover an open tray menu. Topmost
    // for the toolbar is owned by EnsureSmallWindowsTopmost / lazy pin order.
    SetLastError(0);
    const BOOL ok = SetWindowPos(hwnd, nullptr, posX, posY, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    ::FTB_HOST_INSETS_PX = shadowInsets;
    ::FTB_HOST_INSETS_VALID = true;
    ClipFloatingToolbarToVisibleScreens(hwnd);
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().Present();
    }
    (void)ok;
}

void ScheduleFloatingToolbarDpiRemeasure(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    KillTimer(hwnd, TIMER_ID_FTB_DPI_REMEASURE);
    SetTimer(hwnd, TIMER_ID_FTB_DPI_REMEASURE, 1, nullptr);
}

void PlaceFloatingToolbarOnScreen(HWND hwnd)
{
    LayoutFloatingToolbar(hwnd, true);
}

// Auto-hide / auto-dim state. All of it lives on the UI thread, like the
// applies. g_ftb_auto_hidden survives focus changes and config syncs on
// purpose: only a toolbar-visible input state change or a settings change
// brings the toolbar back, otherwise every focus switch would re-show it.
bool g_ftb_auto_hidden = false;
bool g_ftb_auto_hide_armed = false;
// At most one fade runs at a time. The kind decides what "finished" means:
// Dim parks at the reduced opacity, Hide and AutoHide both take the host off
// screen — only AutoHide counts as auto-hidden, so coming back from it takes a
// state change, not merely the next visibility decision.
enum class FtbFadeKind
{
    None,
    Dim,
    Hide,
    AutoHide,
};
FtbFadeKind g_ftb_fade_kind = FtbFadeKind::None;
// Sitting at the dimmed opacity after a finished dim fade. Hover or a state
// change (RevealAutoHiddenFloatingToolbar / RestartFloatingToolbarAutoHide)
// brings the base opacity back.
bool g_ftb_auto_dimmed = false;
ULONGLONG g_ftb_auto_hide_deadline = 0;
ULONGLONG g_ftb_auto_dim_deadline = 0;
ULONGLONG g_ftb_fade_start = 0;
BYTE g_ftb_fade_from = 255;
BYTE g_ftb_fade_target = 255;
// Last alpha handed to the presenter. A fade starts from here rather than from
// 255, so a hide that fires while dimmed fades from the dimmed value down.
BYTE g_ftb_current_alpha = 255;

ULONGLONG FloatingToolbarAutoHideDelayMs()
{
    return static_cast<ULONGLONG>(GetConfiguredFloatingToolbarAutoHideDelay()) * 1000ULL;
}

ULONGLONG FloatingToolbarAutoDimDelayMs()
{
    return static_cast<ULONGLONG>(GetConfiguredFloatingToolbarAutoDimDelay()) * 1000ULL;
}

// 100% maps to 255, so an unset config keeps the fully opaque host.
// D2D 端前景/背景不透明度落在绘制颜色里（FloatingToolbarPresenter::ApplyTheme），
// 窗口级 alpha 只服务自动降低与隐藏淡出，基准是"不压"：整窗 alpha 把图标与文字
// 一起乘掉，把背景不透明度写进来等于把前景也调低。
BYTE FloatingToolbarBaseAlpha()
{
    return FanyImeUi::kWindowBaseAlpha;
}

BYTE FloatingToolbarDimAlpha()
{
    return FanyImeUi::WindowOpacityAlpha(GetConfiguredFloatingToolbarAutoDimOpacity());
}

void SetFloatingToolbarAlpha(BYTE alpha)
{
    // The host has no redirection surface, so the fade is drawn by the
    // presenter rather than applied as a layered-window alpha. A lost alpha
    // would leave an invisible toolbar, so every path that ends a fade puts
    // 255 back.
    // 写的是整窗不透明度，前景也在里面：只有渐变（自动降低、隐藏）和"放回基准"
    // 两种语义，不要把前景/背景不透明度直接传进来。
    g_ftb_current_alpha = alpha;
    FloatingToolbarPresenter::Instance().SetOpacity(static_cast<float>(alpha) / 255.0f);
}

// Set when a finished fade hid the host at alpha 0; the next show owes it the
// base alpha.
bool g_ftb_alpha_cleared = false;

void RestoreFloatingToolbarAlpha()
{
    if (g_ftb_alpha_cleared)
    {
        g_ftb_alpha_cleared = false;
        SetFloatingToolbarAlpha(FloatingToolbarBaseAlpha());
    }
}

// GetCursorPos and GetWindowRect are both physical pixels under per-monitor-v2,
// so this needs no DPI conversion. Sampling the rect instead of tracking mouse
// messages also covers the caption drag area. A window region (an image-mode
// skin's shape, or the part left on screen) narrows it: the cursor over a
// clipped-away pixel is over whatever lies behind.
bool IsCursorOverFloatingToolbar()
{
    POINT cursor{};
    RECT rect{};
    if (!GetCursorPos(&cursor) || !GetWindowRect(::global_hwnd_ftb, &rect) || !PtInRect(&rect, cursor))
    {
        return false;
    }
    bool inside = true;
    if (HRGN region = CreateRectRgn(0, 0, 0, 0))
    {
        if (GetWindowRgn(::global_hwnd_ftb, region) != ERROR)
        {
            inside = PtInRegion(region, cursor.x - rect.left, cursor.y - rect.top) != FALSE;
        }
        DeleteObject(region);
    }
    return inside;
}

// Start a fade from the current alpha. Callers never interrupt a hide with a
// different kind — the hide owns the host until the next show — and a dim that
// is still running is simply restarted from wherever the alpha is.
void BeginFloatingToolbarFade(BYTE target, FtbFadeKind kind)
{
    if (!::global_hwnd_ftb)
    {
        return;
    }
    if ((g_ftb_fade_kind == FtbFadeKind::Hide || g_ftb_fade_kind == FtbFadeKind::AutoHide) && kind != g_ftb_fade_kind)
    {
        return;
    }
    g_ftb_fade_kind = kind;
    g_ftb_fade_start = GetTickCount64();
    g_ftb_fade_from = g_ftb_current_alpha;
    g_ftb_fade_target = target;
    SetTimer(::global_hwnd_ftb, TIMER_ID_FTB_AUTO_HIDE_FADE, kFloatingToolbarAutoHideFadeStepMs, nullptr);
}

// Any interaction cancels a fade in progress and pulls a dimmed toolbar back
// to the base opacity. With neither a fade nor a dim in flight this leaves the
// alpha alone, so the hide-at-0 handoff via g_ftb_alpha_cleared still works.
void CancelFloatingToolbarFade()
{
    if (g_ftb_fade_kind == FtbFadeKind::None && !g_ftb_auto_dimmed)
    {
        return;
    }
    KillTimer(::global_hwnd_ftb, TIMER_ID_FTB_AUTO_HIDE_FADE);
    g_ftb_fade_kind = FtbFadeKind::None;
    g_ftb_auto_dimmed = false;
    SetFloatingToolbarAlpha(FloatingToolbarBaseAlpha());
}

// The synchronous half of HideFloatingToolbarHost: no fade, no cloak decision.
// Called by the fade's finish path (which already owns the alpha) and by the
// paths that have nothing to fade.
void HideFloatingToolbarHostNow()
{
    if (!::global_hwnd_ftb)
    {
        return;
    }
    ShowWindow(::global_hwnd_ftb, SW_HIDE);
}

void DisarmFloatingToolbarAutoHide()
{
    if (!::global_hwnd_ftb)
    {
        return;
    }
    KillTimer(::global_hwnd_ftb, TIMER_ID_FTB_AUTO_HIDE);
    g_ftb_auto_hide_armed = false;
    CancelFloatingToolbarFade();
}

// Starts a fresh countdown. Callers decide whether an already-running one
// should be restarted; a mere re-apply (focus change) keeps it running.
void ArmFloatingToolbarAutoHide()
{
    if (!::global_hwnd_ftb || (!GetConfiguredFloatingToolbarAutoHide() && !GetConfiguredFloatingToolbarAutoDim()))
    {
        return;
    }
    // A fresh countdown is itself an interaction: a dimmed toolbar comes back
    // to the base opacity and the countdown restarts from there.
    CancelFloatingToolbarFade();
    const ULONGLONG now = GetTickCount64();
    g_ftb_auto_hide_deadline = now + FloatingToolbarAutoHideDelayMs();
    g_ftb_auto_dim_deadline = now + FloatingToolbarAutoDimDelayMs();
    if (!g_ftb_auto_hide_armed)
    {
        SetTimer(::global_hwnd_ftb, TIMER_ID_FTB_AUTO_HIDE, kFloatingToolbarAutoHidePollMs, nullptr);
        g_ftb_auto_hide_armed = true;
    }
}

void PollFloatingToolbarAutoHide()
{
    const bool auto_hide = GetConfiguredFloatingToolbarAutoHide();
    const bool auto_dim = GetConfiguredFloatingToolbarAutoDim();
    if ((!auto_hide && !auto_dim) || !IsWindowVisible(::global_hwnd_ftb))
    {
        DisarmFloatingToolbarAutoHide();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    // Hover is interaction. A toolbar that is still cloaked would make the
    // hide a no-op, so the countdown only runs on a settled toolbar.
    if (IsCursorOverFloatingToolbar() || !FloatingToolbarPresenter::Instance().IsBound() ||
        IsHostWindowCloaked(::global_hwnd_ftb))
    {
        // 悬停即交互：在途的淡出取消，已经停在降低值的拉回基准。
        CancelFloatingToolbarFade();
        g_ftb_auto_hide_deadline = now + FloatingToolbarAutoHideDelayMs();
        g_ftb_auto_dim_deadline = now + FloatingToolbarAutoDimDelayMs();
        return;
    }
    // Dim goes first: it is the gentler of the two, and a hide that fires
    // afterwards fades from the dimmed alpha instead of the base one. Both
    // wait for a quiet fade slot so two fades never share one timer.
    if (auto_dim && !g_ftb_auto_dimmed && g_ftb_fade_kind == FtbFadeKind::None && now >= g_ftb_auto_dim_deadline)
    {
        FTB_DIAG_LOGF(L"ftb auto-dim fade started delay={}s target={}%", GetConfiguredFloatingToolbarAutoDimDelay(),
                      GetConfiguredFloatingToolbarAutoDimOpacity());
        BeginFloatingToolbarFade(FloatingToolbarDimAlpha(), FtbFadeKind::Dim);
        return;
    }
    if (auto_hide && g_ftb_fade_kind == FtbFadeKind::None && now >= g_ftb_auto_hide_deadline)
    {
        FTB_DIAG_LOGF(L"ftb auto-hide fade started delay={}s", GetConfiguredFloatingToolbarAutoHideDelay());
        BeginFloatingToolbarFade(0, FtbFadeKind::AutoHide);
    }
}

void StepFloatingToolbarFade()
{
    if (g_ftb_fade_kind == FtbFadeKind::None)
    {
        KillTimer(::global_hwnd_ftb, TIMER_ID_FTB_AUTO_HIDE_FADE);
        return;
    }
    if (!IsWindowVisible(::global_hwnd_ftb))
    {
        // Hidden underneath the fade by someone else (fullscreen hook).
        CancelFloatingToolbarFade();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (IsCursorOverFloatingToolbar())
    {
        CancelFloatingToolbarFade();
        g_ftb_auto_hide_deadline = now + FloatingToolbarAutoHideDelayMs();
        g_ftb_auto_dim_deadline = now + FloatingToolbarAutoDimDelayMs();
        return;
    }
    const ULONGLONG elapsed = now - g_ftb_fade_start;
    // 普通隐藏淡出短一档：它常常跟着焦点切换，淡久了窗口会"黏"在刚切换走的
    // 应用上。自动隐藏/自动降低沿用既有的长淡出节奏。
    const UINT fadeMs =
        g_ftb_fade_kind == FtbFadeKind::Hide ? kFloatingToolbarHideFadeMs : kFloatingToolbarAutoHideFadeMs;
    if (elapsed < fadeMs)
    {
        SetFloatingToolbarAlpha(FanyImeUi::WindowFadeAlpha(g_ftb_fade_from, g_ftb_fade_target, elapsed, fadeMs));
        return;
    }
    if (g_ftb_fade_kind == FtbFadeKind::Dim)
    {
        // Dimmed, not hidden: the toolbar stays on screen at the reduced
        // opacity until hover or a state change brings the base opacity back.
        FTB_DIAG_LOGF(L"ftb auto-dim fade finished");
        g_ftb_fade_kind = FtbFadeKind::None;
        g_ftb_auto_dimmed = true;
        SetFloatingToolbarAlpha(g_ftb_fade_target);
        return;
    }
    // Hide / AutoHide finished. The kind only matters for g_ftb_auto_hidden:
    // an auto-hidden toolbar needs a state change to come back, a plainly
    // hidden one returns with the next visibility decision.
    const bool from_auto_hide = g_ftb_fade_kind == FtbFadeKind::AutoHide;
    KillTimer(::global_hwnd_ftb, TIMER_ID_FTB_AUTO_HIDE_FADE);
    g_ftb_fade_kind = FtbFadeKind::None;
    // The hide supersedes a dim: clearing it here keeps Disarm's cancel from
    // painting the base alpha back over the alpha-0 hide below.
    g_ftb_auto_dimmed = false;
    // Hide at alpha 0 and leave it there: restoring the base alpha here
    // (directly, or via the cancel path in Disarm) paints one opaque frame
    // before or during the DWM hide transition. The next show restores the
    // alpha instead.
    SetFloatingToolbarAlpha(0);
    g_ftb_alpha_cleared = true;
    if (from_auto_hide)
    {
        FTB_DIAG_LOGF(L"ftb auto-hide fade finished");
        g_ftb_auto_hidden = true;
        DisarmFloatingToolbarAutoHide();
    }
    else
    {
        FTB_DIAG_LOGF(L"ftb hide fade finished");
    }
    HideFloatingToolbarHostNow();
    if (IsWindowVisible(::global_hwnd_ftb) && !IsHostWindowCloaked(::global_hwnd_ftb))
    {
        // The hide did not take, so the host is still on screen.
        RestoreFloatingToolbarAlpha();
    }
}
} // namespace

// Apply the configured toolbar opacity: at window creation, and from the config
// sync when an opacity setting changes. The two values land in the drawn colors,
// so this re-resolves the skin and repaints; there is no fade, exactly like the
// other appearance items (font size, icon size).
void ApplyConfiguredFloatingToolbarOpacity()
{
    if (!::global_hwnd_ftb)
    {
        return;
    }
    // 只有 ApplyTheme() 会重新解析皮肤颜色再重建场景（ApplyAppearance 只是拿缓存下来
    // 的 impl_->fill / impl_->glyph 重建，改值到不了画面上）。这里必须走 ApplyTheme()，
    // 否则两个不透明度选项点了没反应，只剩窗口级 alpha 那条路把整窗（含图标文字）压暗。
    FloatingToolbarPresenter::Instance().ApplyTheme();
}

void RestartFloatingToolbarAutoHide(const wchar_t *reason)
{
    if (!::global_hwnd_ftb)
    {
        return;
    }
    g_ftb_auto_hidden = false;
    DisarmFloatingToolbarAutoHide();
    ApplyConfiguredFloatingToolbarVisibility(reason);
}

void RevealAutoHiddenFloatingToolbar(const wchar_t *reason)
{
    if (!::global_hwnd_ftb || (!GetConfiguredFloatingToolbarAutoHide() && !GetConfiguredFloatingToolbarAutoDim()))
    {
        return;
    }
    if (g_ftb_auto_hidden)
    {
        g_ftb_auto_hidden = false;
        ApplyConfiguredFloatingToolbarVisibility(reason);
    }
    else if (g_ftb_auto_dimmed)
    {
        // 自动降低不透明度待机中：状态变化即交互，拉回基准不透明度；
        // Arm 在下面重新开始两个计时。
        CancelFloatingToolbarFade();
    }
    if (IsWindowVisible(::global_hwnd_ftb))
    {
        ArmFloatingToolbarAutoHide();
    }
}

void ApplyConfiguredFloatingToolbarVisibility(const wchar_t *reason)
{
    if (!::global_hwnd_ftb)
    {
        FTB_DIAG_LOGF(L"apply reason={} skipped: toolbar window not created yet", reason);
        return;
    }
    // IPC ownership and this flag can only disagree in one direction. Terminal
    // deactivation clears the active client, so a live client alongside an
    // inactive flag is never legitimate: it means a WM_IMEACTIVATE edge was
    // lost, most often because the activation raced candidate-window creation.
    // Nothing else re-evaluates visibility afterwards, so the toolbar would stay
    // hidden until the user changed focus again.
    const uint64_t active_client = GetActivePipeClient().client_id;
    if (active_client != 0 && !g_is_ime_active)
    {
        FTB_DIAG_LOGF(L"reconcile reason={} active_client={} repairs ime_active false -> true", reason, active_client);
        g_is_ime_active = true;
    }

    const HWND foreground = GetForegroundWindow();
    const bool fullscreen = foreground && CheckFullscreen(foreground);
    const bool configured = GetConfiguredFloatingToolbarEnabled();
    // Switching the feature off must not leave the toolbar stuck hidden.
    if (!GetConfiguredFloatingToolbarAutoHide())
    {
        g_ftb_auto_hidden = false;
    }
    const bool should_show =
        FanyImeUi::ShouldShowFloatingToolbar(configured, fullscreen, g_is_ime_active, g_ftb_auto_hidden);
    const bool is_visible = IsWindowVisible(::global_hwnd_ftb) != FALSE;
    const bool presenter_ready = FloatingToolbarPresenter::Instance().IsBound();

    // Each host-state snapshot crosses into DWM and walks the child windows, and
    // a burst of queued activations can drive hundreds of applies through here on
    // the UI thread. Collapsing identical consecutive records keeps the trace
    // complete without letting the diagnostics starve the UI thread.
    bool trace = false;
    if (::DiagnosticLog::IsEnabled())
    {
        // Every input of the decision, so a blank toolbar can be attributed to a
        // specific term rather than guessed at. Cloak and presenter readiness
        // matter more than IsWindowVisible once the host exists.
        std::wstring decision =
            fmt::format(L"apply reason={} active_client={} ime_active={} configured={} fullscreen={} "
                        L"auto_hidden={} should_show={} was_visible={} was_cloaked={} presenter_ready={}",
                        reason, active_client, g_is_ime_active, configured, fullscreen, g_ftb_auto_hidden, should_show,
                        is_visible, IsHostWindowCloaked(::global_hwnd_ftb), presenter_ready);

        // Applies are confined to the UI thread, so plain statics suffice.
        static std::wstring last_decision;
        static unsigned long long repeats = 0;
        trace = decision != last_decision;
        if (!trace)
        {
            ++repeats;
        }
        else
        {
            if (repeats != 0)
            {
                FTB_DIAG_LOGF(L"  (preceding apply record repeated {} more times)", repeats);
                repeats = 0;
            }
            last_decision = decision;
            ::DiagnosticLog::Write(decision);
            // The decision above only explains a hidden toolbar. A blank one
            // needs the host state, which nothing else reports.
            FTB_DIAG_LOGF(L"  state before reason={} {}", reason, DescribeFloatingToolbarHostState());
        }
    }
    // Whatever the decision was, record what it actually produced. A show that
    // leaves the state unchanged is the failure being hunted here.
    struct StateAfter
    {
        const wchar_t *reason;
        bool trace;
        ~StateAfter()
        {
            if (trace)
            {
                FTB_DIAG_LOGF(L"  state after  reason={} {}", reason, DescribeFloatingToolbarHostState());
            }
        }
    } state_after{reason, trace};

    if (should_show)
    {
        // Keep the dragged position across IME activate / config / fullscreen
        // exit. Only resize for the current DPI.
        LayoutFloatingToolbar(::global_hwnd_ftb, false);
        EnsureSmallWindowsTopmost(L"show-floating-toolbar");
        // Ensure may pin FTB last while the tray menu is open; put the menu
        // back on top without a visible flash. IsWindowVisible() cannot express
        // "open" here: the menu host spends all of startup visible-but-cloaked.
        if (IsTrayMenuOpenToUser())
        {
            RaiseTrayMenuAboveSmallWindows(L"after-show-floating-toolbar");
        }
        // 淡出途中收到 show（停用中又激活、自动隐藏淡到一半）：拉回基准不透明度，
        // 否则淡完会把刚要显示的工具栏藏掉，自动隐藏还会顺手置上 auto_hidden。
        // 不透明度调节渐变除外——让它自然结束更顺。
        if (g_ftb_fade_kind == FtbFadeKind::Hide || g_ftb_fade_kind == FtbFadeKind::AutoHide)
        {
            CancelFloatingToolbarFade();
        }
        // Still hidden here, so putting the alpha back cannot flash.
        RestoreFloatingToolbarAlpha();
        if (!is_visible)
        {
            ShowWindow(::global_hwnd_ftb, SW_SHOWNA);
        }
        // Reveal only once there is something to reveal: without a presenter
        // uncloaking would just park an empty transparent rectangle on screen.
        if (presenter_ready)
        {
            SetHostWindowCloaked(::global_hwnd_ftb, false);
        }
        // A toolbar coming back from hidden counts down afresh; one that was
        // already up keeps its running countdown across repeated applies.
        if (!is_visible || !g_ftb_auto_hide_armed)
        {
            ArmFloatingToolbarAutoHide();
        }
    }
    else
    {
        if (g_ftb_fade_kind == FtbFadeKind::Hide || g_ftb_fade_kind == FtbFadeKind::AutoHide)
        {
            // 淡出已在途中：它自己会收尾（含 disarm 与 auto_hidden）。在这儿
            // 取消只会让工具栏弹回基准透明度、再从头淡一次。
            FTB_DIAG_LOGF(L"hide decision folded into in-flight fade");
        }
        else
        {
            DisarmFloatingToolbarAutoHide();
            if (is_visible)
            {
                HideFloatingToolbarHost();
            }
        }
    }
}

// A toolbar whose presenter never came up has nothing to hide; keep it cloaked
// so it stays out of the way.
void HideFloatingToolbarHost()
{
    if (!::global_hwnd_ftb)
    {
        return;
    }
    // The fullscreen hook calls this directly, so without a line here the trace
    // shows a toolbar that became hidden with no apply to account for it.
    const bool presenter_ready = FloatingToolbarPresenter::Instance().IsBound();
    FTB_DIAG_LOGF(L"hide host presenter_ready={}", presenter_ready);
    if (!presenter_ready)
    {
        SetHostWindowCloaked(::global_hwnd_ftb, true);
        return;
    }
    // 已经挂在淡出上（重复隐藏被它吸收）、或者本来就没人可淡（已隐藏 / 未上
    // 屏）都直接收尾，不重开一段淡出。
    if (g_ftb_fade_kind == FtbFadeKind::Hide || g_ftb_fade_kind == FtbFadeKind::AutoHide ||
        !IsWindowVisible(::global_hwnd_ftb) || IsHostWindowCloaked(::global_hwnd_ftb))
    {
        HideFloatingToolbarHostNow();
        return;
    }
    // 独占全屏一帧都不能多：全屏钩子与"前台全屏就不该显示"这两条路都走这里，
    // 200ms 的淡出足够让一个分层窗口盖在游戏上把它踢出独占模式（候选窗同样
    // 直接收尾）。全屏判定只在真的要淡出时付一次。
    const HWND foreground = GetForegroundWindow();
    if (!foreground || CheckFullscreen(foreground))
    {
        HideFloatingToolbarHostNow();
        return;
    }
    // 普通隐藏也淡出：停用、切窗口都走这里，200ms 淡完再由定时器收尾。期间
    // 悬停或一次 show 都会把淡出取消掉。
    FTB_DIAG_LOGF(L"ftb hide fade started");
    BeginFloatingToolbarFade(0, FtbFadeKind::Hide);
}

void ApplyConfiguredFloatingToolbarSize()
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyAppearance();
        return;
    }
    ::FTB_WND_WIDTH = ConfiguredFloatingToolbarWidth();
    ::FTB_WND_HEIGHT = ConfiguredFloatingToolbarHeight();
    LayoutFloatingToolbar(::global_hwnd_ftb, false);
}

namespace
{
void RemeasureFloatingToolbarAfterDpiChange(HWND /*hwnd*/)
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().RelayoutHost();
    }
}
} // namespace

LRESULT CALLBACK WndProcFtbWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // The floating toolbar is persistent, so it cannot rely on being re-themed
    // on the next show (unlike the tray menu, which re-themes on every open).
    if (IsSystemLightDarkToggle(message, lParam) && FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyTheme();
    }

    if (message == WM_NCHITTEST && FloatingToolbarPresenter::Instance().IsBound())
    {
        POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &client);
        if (FloatingToolbarPresenter::Instance().HitCaptionDrag(client))
        {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    if (FloatingToolbarPresenter::Instance().HandleMessage(message, wParam, lParam))
    {
        return message == WM_ERASEBKGND ? 1 : 0;
    }

    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case UPDATE_FTB_STATUS: {
        int capsLockState = (wParam >> 3) & 0x1;
        int cnEnState = (wParam >> 2) & 0x1;
        int doubleSingleByteState = (wParam >> 1) & 0x1;
        int puncState = wParam & 0x1;
        UpdateFtbCnEnAndDoubleSingleAndPuncState(cnEnState, doubleSingleByteState, puncState, capsLockState);
        break;
    }

    case UPDATE_FTB_ENGLISH_INPUT_MODE:
        UpdateFtbEnglishInputModeState(wParam != 0 ? 1 : 0);
        break;

    case UPDATE_FTB_INPUT_MODE:
        UpdateFtbInputModeState(wParam != 0 ? 1 : 0);
        break;

    case UPDATE_FTB_CAPS_LOCK:
        UpdateFtbCapsLockState(wParam != 0 ? 1 : 0);
        BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::CapsLockChanged,
                                               wParam != 0 ? L"1" : L"0");
        break;

    case WM_EXITSIZEMOVE:
        // Native caption dragging runs a modal move loop. Clamp only after that
        // loop ends so movement remains smooth and crossing to another monitor
        // is never blocked. A remeasure still pending from a mid-drag DPI change
        // runs first, so the clamp sees the final size and the toolbar does not
        // jump a second time when the timer fires.
        if (KillTimer(hwnd, TIMER_ID_FTB_DPI_REMEASURE))
        {
            RemeasureFloatingToolbarAfterDpiChange(hwnd);
        }
        KeepFloatingToolbarInsideVisibleScreens(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGED: {
        // Follow the caption drag live, so the shadow shows again as soon as the
        // bar leaves an edge and is trimmed as soon as it reaches one.
        const auto *pos = reinterpret_cast<const WINDOWPOS *>(lParam);
        if (pos && (pos->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE))
        {
            ClipFloatingToolbarToVisibleScreens(hwnd);
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    case WM_DPICHANGED: {
        const FLOAT scale = HIWORD(wParam) / 96.0f;
        // Apply Windows' recommended placement, also mid-drag: it keeps the
        // toolbar under the cursor so the DPI does not flip back and forth.
        const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
        if (FloatingToolbarPresenter::Instance().IsBound())
        {
            FloatingToolbarPresenter::Instance().RelayoutHost(scale, suggested);
            ScheduleFloatingToolbarDpiRemeasure(hwnd);
            return 0;
        }
        // Size from DIPs * the DPI in wParam (GetWindowScale can lag).
        ::FTB_CONTENT_WIDTH_DIP = 0.0;
        ::FTB_CONTENT_HEIGHT_DIP = 0.0;
        LayoutFloatingToolbar(hwnd, false, scale > 0.0f ? scale : 0.0f, suggested);
        ScheduleFloatingToolbarDpiRemeasure(hwnd);
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        // Resolution-only switches may not send WM_DPICHANGED. Reclamp + remasure.
        ScheduleFloatingToolbarDpiRemeasure(hwnd);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_ID_PLACE_FTB)
        {
            KillTimer(hwnd, TIMER_ID_PLACE_FTB);
            PlaceFloatingToolbarOnScreen(hwnd);
            break;
        }
        if (wParam == TIMER_ID_FTB_AUTO_HIDE)
        {
            PollFloatingToolbarAutoHide();
            break;
        }
        if (wParam == TIMER_ID_FTB_AUTO_HIDE_FADE)
        {
            StepFloatingToolbarFade();
            break;
        }
        if (wParam == TIMER_ID_FTB_DPI_REMEASURE)
        {
            KillTimer(hwnd, TIMER_ID_FTB_DPI_REMEASURE);
            RemeasureFloatingToolbarAfterDpiChange(hwnd);
            break;
        }
        break;
    }
    default: {
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    }
    return 0;
}
