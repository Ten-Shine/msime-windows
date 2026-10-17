#pragma once
// 候选窗与悬浮工具栏共用的窗口不透明度换算。配置里存的是百分比，两条写入路径各
// 取一种形式：前景/背景不透明度落在**绘制颜色**上（0–1 的浮点），而整窗 alpha
// ——隐藏淡出与自动降低不透明度——走分层窗口的 LWA_ALPHA 字节。D2D 宿主没有
// 重定向位图（WS_EX_NOREDIRECTIONBITMAP），除了整帧效果拿不到窗口级 alpha。
#include <algorithm>
#include <cstdint>

namespace FanyImeUi
{
// 下限 1% 是刻意放开到底的：再低窗口就只剩一个影子，但用户要的就是"几乎
// 看不见"，步进器也能随时调回来，这里不替他做判断。
constexpr int kWindowOpacityPercentMin = 1;
constexpr int kWindowOpacityPercentMax = 100;
// 100 = 完全不透明，也就是这个功能加入前的默认外观。
constexpr int kWindowOpacityPercentDefault = 100;

constexpr int ClampWindowOpacityPercent(int percent)
{
    return (std::max)(kWindowOpacityPercentMin, (std::min)(kWindowOpacityPercentMax, percent));
}

// 百分比 -> LWA_ALPHA 字节，四舍五入到最近的字节值。
constexpr std::uint8_t WindowOpacityAlpha(int percent)
{
    return static_cast<std::uint8_t>((ClampWindowOpacityPercent(percent) * 255 + 50) / 100);
}

// 百分比 -> D2D 绘制颜色的浮点不透明度系数。
constexpr float WindowOpacityFloat(int percent)
{
    return static_cast<float>(ClampWindowOpacityPercent(percent)) / 100.0f;
}

// 整窗 alpha 的基准值：恒为"不压"。窗口级 alpha 乘在**整帧**上，前景也在里面，
// 所以它只能承载渐变这种整体效果，不能承载背景不透明度——把它写成背景值，前景就
// 会被背景一起乘掉，调低背景等于把文字也调低。前景/背景不透明度落在绘制颜色里
// （CandidatePresenter::ApplySkin、FloatingToolbarPresenter::ApplyTheme）。
constexpr std::uint8_t kWindowBaseAlpha = 255;

// 渐变的插值：在 from→target 之间按**已过去**的时间比例取值。
//
// 务必用 elapsed，不要用 duration - elapsed（剩余比例）：那样曲线会整个反过来——第一帧
// 直接落在目标值上，随后沿反方向一路爬回起点，最后才落回目标。自动降低不透明度的体感就是
// "先暗一下、再花整个淡出时长亮回原状、最后突然暗下去"，而隐藏淡出则是"先几乎全透明、
// 再亮回来、最后才消失"。duration 为 0 或已过时直接给目标值。
constexpr std::uint8_t WindowFadeAlpha(std::uint8_t from, std::uint8_t target, std::uint64_t elapsed,
                                       std::uint64_t duration)
{
    if (duration == 0 || elapsed >= duration)
    {
        return target;
    }
    const std::int64_t from64 = static_cast<std::int64_t>(from);
    const std::int64_t span = static_cast<std::int64_t>(target) - from64;
    const std::int64_t value = from64 + span * static_cast<std::int64_t>(elapsed) / static_cast<std::int64_t>(duration);
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}
} // namespace FanyImeUi
