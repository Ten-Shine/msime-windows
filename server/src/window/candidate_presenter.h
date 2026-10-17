#pragma once

#include "utils/window_utils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <windows.h>

class CandidatePresenter
{
  public:
    static CandidatePresenter &Instance();

    bool Bind(HWND hwnd);
    bool IsBound() const;
    // Re-resolve the skin tokens and repaint. Public because the window layer
    // calls it when the foreground/background opacity change: on D2D those two
    // land in the drawn colors, not in a window attribute.
    void ApplySkin();
    // Whole-frame opacity (0..1) used while fading the window out. D2D has no
    // window-level alpha to lean on (the host has no redirection bitmap), so
    // the fade is drawn as a layer opacity around the scene instead.
    void SetFrameOpacity(float opacity);
    void ShowFromGlobalState();
    void ShowFromGlobalState(POINT caret);
    void Hide();
    void Present();
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

  private:
    CandidatePresenter();
    ~CandidatePresenter();
    CandidatePresenter(const CandidatePresenter &) = delete;
    CandidatePresenter &operator=(const CandidatePresenter &) = delete;

    void RebuildScene();
    // Returns the generation of the snapshot that was actually rendered, so the caller can echo it
    // back to Global::rendered_candidate_page_generation once the paint is on screen.
    std::uint64_t FillItemsFromUi();
    // `scale` carries the scale resolved once per show in ShowFromGlobalState
    // so measure, clamping, sizing and rendering all share one source; an
    // unset (scale == 0) value makes PlaceAndShow resolve it itself.
    void PlaceAndShow(POINT caret, float widthDip, float heightDip, float cardLeftDip, float cardTopDip,
                      const ResolvedCandidateScale &scale = {});
    // Hide leaves the host cloaked; clearing it keeps the next uncloak from
    // briefly showing the previous card.
    void PresentEmptyFrame();
    void ArmHoverIfPointerMoved();
    void CommitItem(size_t pageIndex);
    void ShowItemContextMenu(size_t pageIndex, POINT clientPoint);
    void CloseContextMenu(bool restoreHost);
    void OpenFixSubmenu();
    void CloseFixSubmenu();
    void ExpandHostForMenu(POINT clientPoint);
    void RestoreHostAfterMenu();
    // 动图的帧时钟：Present 之后按场景给出的下一次换帧时刻排一个一次性定时器，到点再 Present。
    // 只在候选窗显示时排；隐藏、换皮肤、窗口销毁时停。
    void ScheduleAnimationFrame();
    void StopAnimationFrames();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    HWND hwnd_ = nullptr;
    bool bound_ = false;
    bool hoverArmed_ = false;
    bool ignoreSelectionCallback_ = false;
    // Set while PlaceAndShow resizes the host; it presents right afterwards.
    bool placingHost_ = false;
    POINT hoverBaseline_{};
    float decorationTopDip_ = 0.0f;
    float decorationWidthDip_ = 0.0f;
    // 卡片最小宽度：默认 160，外部皮肤的 min_width_dip 与装饰图宽度可以把它撑大，
    // 装饰图因此始终落在卡片宽度之内。
    float cardMinWidthDip_ = 160.0f;
    // 皮肤开了 fit_content：没有默认的 160，卡片宽度也不在同一输入里只增不减。
    bool fitContent_ = false;
    // 只有 v2（canvas）皮肤播放动图；v1 皮肤的图片保持原先的静态首帧。
    bool skinAnimates_ = false;
    // 动画从候选窗这一次显示起算；隐藏或换皮肤后，下一次显示重新从第一帧开始。
    bool animationClockRunning_ = false;
    std::string lastSkinFingerprint_;
    int lastHostWidthPx_ = 0;
    int lastHostHeightPx_ = 0;
    float lastLayoutWidthDip_ = 0.0f;
    float lastLayoutHeightDip_ = 0.0f;
    // Largest card size kept for the current input (preedit + caret + page,
    // stickyCardKey_); a new key or Hide() starts from the natural size.
    std::wstring stickyCardKey_;
    float stickyCardWidthDip_ = 0.0f;
    float stickyCardHeightDip_ = 0.0f;
    int wheelDeltaAccumulator_ = 0;
    // 整帧不透明度（隐藏淡出用）。1.0 时不压图层，走原来的绘制路径。D2D 的图层
    // 本身跟 render target 走，放在 Impl 里。
    float frameOpacity_ = 1.0f;
};
