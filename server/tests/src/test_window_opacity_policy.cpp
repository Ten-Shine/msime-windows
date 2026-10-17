#include "tests/includes/test_framework.h"
#include "window/window_opacity_policy.h"

TEST_CASE(window_opacity_percent_clamps_into_the_supported_range)
{
    REQUIRE(FanyImeUi::ClampWindowOpacityPercent(0) == FanyImeUi::kWindowOpacityPercentMin);
    REQUIRE(FanyImeUi::ClampWindowOpacityPercent(1) == 1);
    REQUIRE(FanyImeUi::ClampWindowOpacityPercent(19) == 19);
    REQUIRE(FanyImeUi::ClampWindowOpacityPercent(60) == 60);
    REQUIRE(FanyImeUi::ClampWindowOpacityPercent(100) == 100);
    REQUIRE(FanyImeUi::ClampWindowOpacityPercent(101) == FanyImeUi::kWindowOpacityPercentMax);
}

TEST_CASE(window_opacity_maps_percent_to_the_layered_alpha_byte)
{
    REQUIRE(FanyImeUi::WindowOpacityAlpha(100) == 255);
    // (50 * 255 + 50) / 100 == 128：半分位落在字节正中间。
    REQUIRE(FanyImeUi::WindowOpacityAlpha(50) == 128);
    // 越界值先收敛到区间端点，再换算。
    REQUIRE(FanyImeUi::WindowOpacityAlpha(0) == FanyImeUi::WindowOpacityAlpha(FanyImeUi::kWindowOpacityPercentMin));
    REQUIRE(FanyImeUi::WindowOpacityAlpha(200) == 255);
}

TEST_CASE(window_opacity_maps_percent_to_the_draw_alpha_float)
{
    REQUIRE(FanyImeUi::WindowOpacityFloat(100) == 1.0f);
    REQUIRE(FanyImeUi::WindowOpacityFloat(50) == 0.5f);
    REQUIRE(FanyImeUi::WindowOpacityFloat(0) == FanyImeUi::WindowOpacityFloat(FanyImeUi::kWindowOpacityPercentMin));
}

// 插值必须按**已过去**的比例走。写成剩余比例会把曲线反过来：首帧直接落在目标值上，随后
// 一路爬回起点，最后才落回目标——自动降低不透明度的体感就是"先暗一下、再亮回原状、
// 最后突然暗下去"（隐藏淡出则是"先全透明、再亮回来、最后才消失"）。
TEST_CASE(window_fade_alpha_walks_from_start_to_target_over_elapsed_time)
{
    // 时长外（含 0）一律给目标值。
    REQUIRE(FanyImeUi::WindowFadeAlpha(255, 77, 0, 400) == 255);
    REQUIRE(FanyImeUi::WindowFadeAlpha(255, 77, 400, 400) == 77);
    REQUIRE(FanyImeUi::WindowFadeAlpha(255, 77, 1, 0) == 77);
    // 中点：255 + (77-255) * 200 / 400 == 255 - 89 == 166。
    REQUIRE(FanyImeUi::WindowFadeAlpha(255, 77, 200, 400) == 166);
    // 反向（隐藏淡出）同理，终点前一次落在 11 附近而不是 244。
    REQUIRE(FanyImeUi::WindowFadeAlpha(255, 0, 200, 400) == 128);
    REQUIRE(FanyImeUi::WindowFadeAlpha(255, 0, 384, 400) == 11);

    // 单调性：目标更低时每 16ms 一步都不允许回升，目标更高时不许回落。
    int previous = -1;
    for (std::uint64_t elapsed = 0; elapsed <= 400; elapsed += 16)
    {
        const int current = FanyImeUi::WindowFadeAlpha(255, 77, elapsed, 400);
        if (elapsed)
        {
            REQUIRE(current <= previous);
        }
        previous = current;
    }
    REQUIRE(previous == 77);
    previous = 256;
    for (std::uint64_t elapsed = 0; elapsed <= 400; elapsed += 16)
    {
        const int current = FanyImeUi::WindowFadeAlpha(77, 255, elapsed, 400);
        if (elapsed)
        {
            REQUIRE(current >= previous);
        }
        previous = current;
    }
    REQUIRE(previous == 255);
}
