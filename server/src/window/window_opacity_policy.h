#pragma once
// 候选窗与悬浮工具栏共用的窗口不透明度换算。配置里存的是百分比，两种渲染后端
// 各取一种形式：WebView2 宿主是分层窗口，直接写 SetLayeredWindowAttributes 的
// LWA_ALPHA 字节；D2D 端没有重定向位图（WS_EX_NOREDIRECTIONBITMAP），前景/背景
// 不透明度落在**绘制颜色**上，要 0–1 的浮点。
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

// 整窗 alpha 的基准值。窗口级 alpha（WebView2 宿主的 LWA_ALPHA、D2D 隐藏淡出的整帧
// 图层）乘在**整帧**上，前景也在里面，所以它只能是"渐变"这种整体效果，不能承载
// 背景不透明度：把它写成背景值，前景就会被背景一起乘掉，调低背景等于把文字也调低。
//
// 原生（D2D）后端的前景/背景不透明度落在绘制颜色里（CandidatePresenter::ApplySkin、
// FloatingToolbarPresenter::ApplyTheme），窗口级 alpha 只服务渐变，基准因此是"不压"。
// WebView2 后端只有整窗一个 alpha，没有前景/背景之分，基准取背景不透明度。
constexpr std::uint8_t WindowOpacityBaseAlpha(bool native_backend, int background_percent)
{
    return native_backend ? 255 : WindowOpacityAlpha(background_percent);
}

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
