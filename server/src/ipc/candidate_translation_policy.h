#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/english/translation_gloss.h"
#include "input_key_policy.h"

namespace FanyImeIpc
{
inline constexpr uint32_t kVirtualKeyReturn = 0x0D;

// Ctrl+Enter commits the translation shown to the right of the highlighted
// candidate instead of the candidate itself. Shift or Alt must not be down:
// those combinations still belong to the application.
constexpr bool IsTranslationCommitKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == kVirtualKeyReturn && (modifiers_down & kKeyModifierMask) == kModifierControl;
}

// The machine-translated zh->en glosses (sources/cn-en/ in msime-dictionary) lead
// each sense with a part-of-speech label: "n. silver medal", "proper n. Wanshan
// District", "adj. spreading; n. sound of laughter". The label is shown in the
// candidate window but must not reach the committed text. The label set and the
// gloss format live in the engine, which needs the same rules to deduplicate the
// glosses it merges from custom_translations.txt.
using translation_gloss::PartOfSpeechLabelLength;
using translation_gloss::StripPartOfSpeechLabel;

enum class TranslationSegmentKind
{
    Text,
    PartOfSpeech,
    Separator,
};

struct TranslationSegment
{
    TranslationSegmentKind kind = TranslationSegmentKind::Text;
    std::string text;
};

// Splits a displayed gloss into the runs the candidate window colors apart: the
// senses' text, their part-of-speech labels and the separators between senses.
// Concatenating the segments' text gives back `gloss` unchanged.
inline std::vector<TranslationSegment> SegmentTranslationGloss(std::string_view gloss)
{
    static constexpr std::string_view kFullwidthSemicolon = "\xEF\xBC\x9B";
    std::vector<TranslationSegment> segments;
    const auto push = [&segments](TranslationSegmentKind kind, std::string_view text) {
        if (text.empty())
            return;
        if (!segments.empty() && segments.back().kind == kind)
            segments.back().text.append(text);
        else
            segments.push_back({kind, std::string(text)});
    };
    const auto pushSense = [&push](std::string_view sense) {
        const size_t first = (std::min)(sense.find_first_not_of(" \t\r\n"), sense.size());
        push(TranslationSegmentKind::Text, sense.substr(0, first));
        sense.remove_prefix(first);
        const size_t label = PartOfSpeechLabelLength(sense);
        push(TranslationSegmentKind::PartOfSpeech, sense.substr(0, label));
        push(TranslationSegmentKind::Text, sense.substr(label));
    };
    size_t start = 0;
    while (start < gloss.size())
    {
        const size_t ascii = gloss.find(';', start);
        const size_t fullwidth = gloss.find(kFullwidthSemicolon, start);
        const size_t cut = (std::min)(ascii, fullwidth);
        if (cut == std::string_view::npos)
        {
            pushSense(gloss.substr(start));
            break;
        }
        pushSense(gloss.substr(start, cut - start));
        const size_t width = cut == fullwidth ? kFullwidthSemicolon.size() : 1;
        push(TranslationSegmentKind::Separator, gloss.substr(cut, width));
        start = cut + width;
    }
    return segments;
}

// Display-only formatting. Keep the original gloss for caching and Ctrl+Enter;
// unlike splitting commit candidates, hiding labels preserves every separator.
inline std::string FormatTranslationGlossForDisplay(std::string_view gloss, bool show_pos)
{
    if (show_pos)
        return std::string(gloss);

    std::string displayed;
    bool after_label = false;
    for (const auto &segment : SegmentTranslationGloss(gloss))
    {
        if (segment.kind == TranslationSegmentKind::PartOfSpeech)
        {
            after_label = true;
            continue;
        }
        std::string_view text = segment.text;
        if (after_label && segment.kind == TranslationSegmentKind::Text)
            text.remove_prefix((std::min)(text.find_first_not_of(" \t\r\n"), text.size()));
        displayed += text;
        after_label = false;
    }
    return displayed;
}

// Helper for the digit-chord recognizers below: the digit range and the exact modifier mask.
constexpr bool IsTranslationDigitFor(uint32_t keycode, uint32_t modifiers_down, uint32_t expected_modifier)
{
    return keycode >= static_cast<uint32_t>('1') && keycode <= static_cast<uint32_t>('9') &&
           (modifiers_down & kKeyModifierMask) == expected_modifier;
}

// Ctrl+1..9 commits the translation of the candidate at that one-based page position, the
// numbered counterpart of Ctrl+Enter (which commits the highlighted one). Only the bare Ctrl
// chord is the IME's: Alt and the Windows keys keep their host meaning, and a bare digit still
// selects the candidate itself. Enabled by default.
constexpr bool IsTranslationDigitKey(uint32_t keycode, uint32_t modifiers_down)
{
    return IsTranslationDigitFor(keycode, modifiers_down, kModifierControl);
}

// Shift+1..9 is the same numbered translation shortcut with Shift instead of Ctrl. It is
// opt-in: by default Shift+digit keeps its host meaning (typically the shifted symbol on
// the key, e.g. Shift+1 types '!'), so this recognizer only says yes when the user enabled
// the shift_digit config key.
constexpr bool IsTranslationShiftDigitKey(uint32_t keycode, uint32_t modifiers_down)
{
    return IsTranslationDigitFor(keycode, modifiers_down, kModifierShift);
}

// Shift+1..9 的译文快捷键，但输入模式自己占用 Shift+数字 时让行。V 模式的裸数字是算式编码键
// （数字、. + - * / ( )），选词改用 Shift+数字，而那里的候选没有译文 —— 抢过来只会让选词不可达。
// TSF 侧 IsShiftDigitOwnedByInputMode 按同一条形状让行（只认主键盘 '1'..'9'）；两边不一致就会变成
// 一次吞键：TSF 把键当 V 模式输入吃下、写进本地预编辑，Server 却当译文快捷键吞掉、不写输入串。
constexpr bool IsTranslationShiftDigitKeyUnlessInputModeOwns(uint32_t keycode, uint32_t modifiers_down,
                                                             bool input_mode_owns_shift_digit)
{
    return IsTranslationShiftDigitKey(keycode, modifiers_down) && !input_mode_owns_shift_digit;
}

// One-based candidate position the Ctrl/Shift+digit chord names, 0 when it is not that chord.
constexpr uint32_t TranslationDigitPosition(uint32_t keycode, uint32_t modifiers_down)
{
    return (IsTranslationDigitKey(keycode, modifiers_down) || IsTranslationShiftDigitKey(keycode, modifiers_down))
               ? keycode - static_cast<uint32_t>('0')
               : 0;
}

// english.db stores one gloss string per entry with the senses already joined:
// en->zh uses the fullwidth '；' ("苹果；家伙"), zh->en the ASCII "; " ("string; trail").
// Every sense is returned, with its part-of-speech label removed.
// A cloud gloss is a single sentence and has no separator, so it stays one entry.
// Header-only pure policy so tests can pin it without linking the server stack.
inline std::vector<std::string> SplitTranslationGloss(std::string_view gloss)
{
    std::vector<std::string> senses;
    for (const auto sense : translation_gloss::SplitSenses(gloss))
        senses.emplace_back(StripPartOfSpeechLabel(sense));
    return senses;
}
} // namespace FanyImeIpc
