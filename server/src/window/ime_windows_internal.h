#pragma once

// ime_windows*.cpp 之间共享的内部声明：拆分前同在 ime_windows.cpp 里、现在跨文件使用的状态、常量、宏与函数。
// 只给 server/src/window/ime_windows*.cpp 包含，其他地方不要引用。

#include <windows.h>
#include <string>
#include "config/ime_config.h"
#include "utils/window_utils.h"
#include "window/small_windows.h"
#include "log/candidate_diag_log.h"

// Places the floating toolbar on the primary monitor's bottom-right slot once
// the host exists.
constexpr UINT_PTR TIMER_ID_PLACE_FTB = 4;
constexpr UINT_PTR TIMER_ID_CONFIG_SYNC = 7;
// Remeasure FTB after live DPI / display-mode changes.
constexpr UINT_PTR TIMER_ID_FTB_DPI_REMEASURE = 10;
constexpr UINT_PTR TIMER_ID_CANDIDATE_MOVE_SETTLE = 11;
constexpr UINT kCandidateMoveSettleMs = 50;
// A hide that is immediately followed by another show is not a teardown: it is
// one keystroke of a burst the worker thread drained late (backspacing the
// composition empty and typing again). Acting on it cloaks a visible candidate
// window and un-cloaks it a few frames later, which is the flicker seen while
// the machine is busy. Defer the hide by this much and drop it outright when a
// show arrives first; a real commit or focus loss still hides, just this many
// milliseconds later.
constexpr UINT_PTR TIMER_ID_CANDIDATE_HIDE_GRACE = 12;
constexpr UINT kCandidateHideGraceMs = 50;
// 候选窗隐藏淡出。隐藏往往跟着一次提交或焦点丢失，淡久了像卡住，所以比工具栏
// 的自动隐藏短；步进间隔与工具栏淡出一致。15 号已经被 caret 兜底占用，这里用 16。
constexpr UINT_PTR TIMER_ID_CANDIDATE_HIDE_FADE = 16;
constexpr UINT kCandidateHideFadeMs = 150;
// Floating toolbar auto-hide / auto-dim. ID 1 on the toolbar HWND belongs to
// msimeui's caret-blink timer. One poll timer drives both countdowns (hide
// first samples hover, then the dim and hide deadlines are checked); one fade
// timer steps the layered alpha, and which fade it is decides what "finished"
// means — dim parks at the reduced opacity, hide takes the host off screen.
constexpr UINT_PTR TIMER_ID_FTB_AUTO_HIDE = 13;
constexpr UINT_PTR TIMER_ID_FTB_AUTO_HIDE_FADE = 14;
constexpr UINT kFloatingToolbarAutoHidePollMs = 200;
constexpr UINT kFloatingToolbarAutoHideFadeStepMs = 16;
constexpr UINT kFloatingToolbarAutoHideFadeMs = 400;
// The DLL sends (0, INVALID_Y) when the host gives no usable text extent. Chromium
// does that for a moment at composition start and then reports the real one, so
// only ask the host for its caret ourselves (window/caret_fallback.h) once the
// anchor has stayed unresolved this long. Firefox's overflowed address bar never
// resolves it (#618).
constexpr UINT_PTR TIMER_ID_CANDIDATE_CARET_FALLBACK = 15;
constexpr UINT kCandidateCaretFallbackDelayMs = 100;
// A failed lookup is retried on a later keystroke, at most this many times per
// composition: the first MSAA/UIA query may only wake the host's accessibility.
constexpr int kCandidateCaretFallbackMaxAttempts = 3;
// When every lookup fails the card's top-left goes to this many physical pixels
// from the top-left of the foreground window's monitor.
constexpr LONG kCandidateCornerFallbackInsetPx = 2;
// 普通隐藏（停用、切窗口）也淡出，但比自动隐藏的 400ms 短：它常常跟着焦点
// 切换，淡久了窗口会"黏"在刚切换走的应用上。独占全屏不淡，直接收尾。
constexpr UINT kFloatingToolbarHideFadeMs = 200;
// A show repeating the previous frame's preedit, page and caret within this
// window is a duplicate post rather than new state — the candidate rebuild and
// the English/cloud merge each request a show per keystroke. Rendering both
// paints the same picture twice. Keep the window short so anything the
// signature does not capture (skin reload, DPI change) self-heals on the next
// keystroke instead of sticking.
constexpr UINT kCandidateShowDedupWindowMs = 250;

int FineTuneWindow(HWND hwnd);
std::wstring DescribeCandidateHostState();

namespace ime_windows_detail
{
extern bool g_is_ime_active;
extern int g_last_placed_caret_x;
extern int g_last_placed_caret_y;
extern bool g_candidate_session_anchor_valid;
extern POINT g_candidate_session_anchor;
extern int g_candidate_dpi_change_suppress_count;
// Last frame actually painted, used to drop duplicate shows. Cleared on hide so
// the next session always paints.
extern std::wstring g_last_rendered_candidate_signature;
extern ULONGLONG g_last_rendered_candidate_tick;

struct SuppressCandidateDpiChange
{
    SuppressCandidateDpiChange()
    {
        ++g_candidate_dpi_change_suppress_count;
    }
    ~SuppressCandidateDpiChange()
    {
        --g_candidate_dpi_change_suppress_count;
    }
    SuppressCandidateDpiChange(const SuppressCandidateDpiChange &) = delete;
    SuppressCandidateDpiChange &operator=(const SuppressCandidateDpiChange &) = delete;
};

// ime_windows.cpp
bool IsSystemLightDarkToggle(UINT message, LPARAM lParam);
void SetHostWindowCloaked(HWND hwnd, bool cloaked);
bool IsHostWindowCloaked(HWND hwnd);
std::wstring DescribeFloatingToolbarHostState();

// 候选窗隐藏淡出。不透明度调节没有渐变——前景/背景不透明度落在绘制颜色里，改值
// 重新套皮肤即可；整帧 alpha 只服务这一条淡出。
extern bool g_candidate_hide_fading;
extern ULONGLONG g_candidate_fade_start;
extern BYTE g_candidate_fade_from;
// 最后一次写入候选宿主的 alpha。淡出从这里起步；结束后由 ApplyCandidateHideNow
// 放回基准值，下一次 show 才不会从 0 开始。
extern BYTE g_candidate_current_alpha;
void BeginCandidateHideFade();
// 取消在途淡出并把 alpha 拉回基准值（交互语义：淡出途中收到 show）。
void CancelCandidateHideFade();
void StepCandidateHideFade(HWND hwnd);
// 候选窗隐藏的收尾：状态复位 + cloak/驻留。隐藏淡出结束、全屏独占和
// "本来就没上屏"的路径都直接走它，不再经过消息处理。
void ApplyCandidateHideNow(HWND hwnd);

// ime_windows_candidate.cpp
POINT GetCandidateLayoutCaret();
// caret 兜底的状态只在 ime_windows_candidate.cpp 里，隐藏收尾在 ime_windows.cpp，
// 由这一处转出去清（停表 + 复位锚点，下一段组合重新判定）。
void ResetCandidateCaretFallback(HWND hwnd);
bool IsCandidateHostPaintedVisible(HWND hwnd);
bool CandidateCaretNeedsStableFollow(HWND hwnd, POINT caret);

// ime_windows_toolbar.cpp
int ConfiguredFloatingToolbarWidth();
int ConfiguredFloatingToolbarHeight();
bool FloatingToolbarItemsEqual(const FloatingToolbarItemsConfig &left, const FloatingToolbarItemsConfig &right);
} // namespace ime_windows_detail
