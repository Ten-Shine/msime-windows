#include "ipc/candidate_translation_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(ctrl_enter_alone_commits_the_secondary_candidate)
{
    using FanyImeIpc::IsTranslationCommitKey;
    REQUIRE(IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl));
    // The UI-less flag lives outside the modifier mask and must not disarm the shortcut.
    REQUIRE(IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierUiLess));

    REQUIRE(!IsTranslationCommitKey(0x0D, 0));
    REQUIRE(!IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierShift));
    REQUIRE(!IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierAlt));
    REQUIRE(!IsTranslationCommitKey(0x20, FanyImeIpc::kModifierControl));
    REQUIRE(!IsTranslationCommitKey('J', FanyImeIpc::kModifierControl));
}

TEST_CASE(ctrl_digit_alone_commits_the_numbered_translation)
{
    using FanyImeIpc::IsTranslationDigitKey;
    using FanyImeIpc::TranslationDigitPosition;
    for (uint32_t digit = '1'; digit <= '9'; ++digit)
    {
        REQUIRE(IsTranslationDigitKey(digit, FanyImeIpc::kModifierControl));
        // The UI-less flag lives outside the modifier mask and must not disarm the shortcut.
        REQUIRE(IsTranslationDigitKey(digit, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierUiLess));
        // Ctrl+1 names the first candidate, so the position is one-based.
        REQUIRE_EQ(TranslationDigitPosition(digit, FanyImeIpc::kModifierControl), digit - '0');
    }

    // A bare digit still selects the candidate itself, and the other modifiers stay the host's.
    REQUIRE(!IsTranslationDigitKey('1', 0));
    REQUIRE(!IsTranslationDigitKey('1', FanyImeIpc::kModifierControl | FanyImeIpc::kModifierShift));
    REQUIRE(!IsTranslationDigitKey('1', FanyImeIpc::kModifierControl | FanyImeIpc::kModifierAlt));
    // 0 is not a candidate position.
    REQUIRE(!IsTranslationDigitKey('0', FanyImeIpc::kModifierControl));
    // A non-digit chord is not a position either.
    REQUIRE(!IsTranslationDigitKey(0x0D, FanyImeIpc::kModifierControl));
    REQUIRE_EQ(TranslationDigitPosition('3', 0), static_cast<uint32_t>(0));
}

TEST_CASE(shift_digit_alone_commits_the_numbered_translation)
{
    using FanyImeIpc::IsTranslationDigitKey;
    using FanyImeIpc::IsTranslationShiftDigitKey;
    using FanyImeIpc::TranslationDigitPosition;
    for (uint32_t digit = '1'; digit <= '9'; ++digit)
    {
        REQUIRE(IsTranslationShiftDigitKey(digit, FanyImeIpc::kModifierShift));
        // The UI-less flag lives outside the modifier mask and must not disarm the shortcut.
        REQUIRE(IsTranslationShiftDigitKey(digit, FanyImeIpc::kModifierShift | FanyImeIpc::kModifierUiLess));
        // Shift+1 names the first candidate, so the position is one-based.
        REQUIRE_EQ(TranslationDigitPosition(digit, FanyImeIpc::kModifierShift), digit - '0');
    }

    // A bare digit still selects the candidate itself, and the other modifiers stay the host's.
    REQUIRE(!IsTranslationShiftDigitKey('1', 0));
    REQUIRE(!IsTranslationShiftDigitKey('1', FanyImeIpc::kModifierShift | FanyImeIpc::kModifierControl));
    REQUIRE(!IsTranslationShiftDigitKey('1', FanyImeIpc::kModifierShift | FanyImeIpc::kModifierAlt));
    // 0 is not a candidate position.
    REQUIRE(!IsTranslationShiftDigitKey('0', FanyImeIpc::kModifierShift));
    // A non-digit chord is not a position either.
    REQUIRE(!IsTranslationShiftDigitKey(0x0D, FanyImeIpc::kModifierShift));
    // Ctrl+digit and Shift+digit are different chords: the position helper recognizes both,
    // but each recognizer only answers for its own modifier.
    REQUIRE(IsTranslationDigitKey('1', FanyImeIpc::kModifierControl));
    REQUIRE(!IsTranslationShiftDigitKey('1', FanyImeIpc::kModifierControl));
    REQUIRE(!IsTranslationDigitKey('1', FanyImeIpc::kModifierShift));
    REQUIRE(IsTranslationShiftDigitKey('1', FanyImeIpc::kModifierShift));
    REQUIRE_EQ(TranslationDigitPosition('3', 0), static_cast<uint32_t>(0));
}

TEST_CASE(translation_shift_digit_stands_down_where_v_mode_owns_the_key)
{
    using FanyImeIpc::IsTranslationDigitKey;
    using FanyImeIpc::IsTranslationShiftDigitKey;
    using FanyImeIpc::IsTranslationShiftDigitKeyUnlessInputModeOwns;
    constexpr uint32_t shift = FanyImeIpc::kModifierShift;
    constexpr uint32_t control = FanyImeIpc::kModifierControl;

    // V-mode composes with bare digits and selects with Shift+1..9, and those candidates
    // carry no gloss, so Shift stands down. The TSF side (IsShiftDigitOwnedByInputMode)
    // matches this shape; a mismatch swallows the key -- TSF eats it as V-mode input, the
    // Server as a translation chord.
    for (uint32_t digit = '1'; digit <= '9'; ++digit)
    {
        REQUIRE(IsTranslationShiftDigitKey(digit, shift));
        REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns(digit, shift, true));
        REQUIRE(IsTranslationShiftDigitKeyUnlessInputModeOwns(digit, shift, false));
    }

    // Ctrl+digit is unaffected: V-mode's input keys never carry Ctrl.
    REQUIRE(IsTranslationDigitKey('8', control));
    REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns('8', control, true));
    REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns('8', control, false));

    // Keys that are not the chord stay rejected either way.
    REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns('8', 0, false));
    REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns('8', shift | control, false));
    REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns('0', shift, false));
    REQUIRE(!IsTranslationShiftDigitKeyUnlessInputModeOwns(0x0D, shift, false));
}

TEST_CASE(translation_gloss_splits_on_both_semicolon_widths)
{
    using FanyImeIpc::SplitTranslationGloss;
    // en->zh glosses join senses with the fullwidth semicolon.
    const auto apple = SplitTranslationGloss("苹果；家伙；[计]苹果公司");
    REQUIRE_EQ(apple.size(), static_cast<size_t>(3));
    REQUIRE_EQ(apple[0], std::string("苹果"));
    REQUIRE_EQ(apple[1], std::string("家伙"));
    REQUIRE_EQ(apple[2], std::string("[计]苹果公司"));

    // zh->en glosses use "; " and the trailing space must not survive.
    const auto chuan = SplitTranslationGloss("string; trail; chuan");
    REQUIRE_EQ(chuan.size(), static_cast<size_t>(3));
    REQUIRE_EQ(chuan[0], std::string("string"));
    REQUIRE_EQ(chuan[1], std::string("trail"));
    REQUIRE_EQ(chuan[2], std::string("chuan"));
}

TEST_CASE(translation_gloss_drops_part_of_speech_labels)
{
    using FanyImeIpc::SplitTranslationGloss;
    // Machine-translated zh->en glosses label each sense; every sense is kept.
    const auto laugh = SplitTranslationGloss("adj. spreading; n. sound of laughter; v. giggle; idiom. burst out");
    REQUIRE_EQ(laugh.size(), static_cast<size_t>(4));
    REQUIRE_EQ(laugh[0], std::string("spreading"));
    REQUIRE_EQ(laugh[1], std::string("sound of laughter"));
    REQUIRE_EQ(laugh[2], std::string("giggle"));
    REQUIRE_EQ(laugh[3], std::string("burst out"));

    const auto place = SplitTranslationGloss("proper n. Wanshan District");
    REQUIRE_EQ(place.size(), static_cast<size_t>(1));
    REQUIRE_EQ(place[0], std::string("Wanshan District"));

    // Unlabelled senses, and words that merely start like a label, are untouched.
    const auto stoop = SplitTranslationGloss("v. stoop; crouch");
    REQUIRE_EQ(stoop[1], std::string("crouch"));
    const auto peculiar = SplitTranslationGloss("very peculiar");
    REQUIRE_EQ(peculiar[0], std::string("very peculiar"));
    const auto bare = SplitTranslationGloss("n.");
    REQUIRE_EQ(bare[0], std::string("n."));
}

namespace
{
std::string DescribeSegments(const std::vector<FanyImeIpc::TranslationSegment> &segments)
{
    std::string out;
    for (const auto &segment : segments)
    {
        const char tag = segment.kind == FanyImeIpc::TranslationSegmentKind::PartOfSpeech ? 'P'
                         : segment.kind == FanyImeIpc::TranslationSegmentKind::Separator  ? 'S'
                                                                                          : 'T';
        out += tag;
        out += '[' + segment.text + ']';
    }
    return out;
}
} // namespace

// The candidate window colors part-of-speech labels and separators apart from the senses. The segments must
// cover the gloss exactly, so the renderers can rebuild the displayed text and its offsets from them.
TEST_CASE(translation_gloss_segments_mark_labels_and_separators)
{
    using FanyImeIpc::SegmentTranslationGloss;
    REQUIRE_EQ(DescribeSegments(SegmentTranslationGloss("adj. spreading; n. sound of laughter")),
               std::string("P[adj.]T[ spreading]S[;]T[ ]P[n.]T[ sound of laughter]"));
    REQUIRE_EQ(DescribeSegments(SegmentTranslationGloss("proper n. Wanshan District")),
               std::string("P[proper n.]T[ Wanshan District]"));
    REQUIRE_EQ(DescribeSegments(SegmentTranslationGloss("苹果；家伙")), std::string("T[苹果]S[；]T[家伙]"));
    // Unlabelled text, a bare label and a trailing separator.
    REQUIRE_EQ(DescribeSegments(SegmentTranslationGloss("very peculiar")), std::string("T[very peculiar]"));
    REQUIRE_EQ(DescribeSegments(SegmentTranslationGloss("n.")), std::string("T[n.]"));
    REQUIRE_EQ(DescribeSegments(SegmentTranslationGloss("v. stoop;")), std::string("P[v.]T[ stoop]S[;]"));
    REQUIRE(SegmentTranslationGloss("").empty());

    std::string joined;
    for (const auto &segment : SegmentTranslationGloss(" v. giggle ；；idiom. burst out; "))
        joined += segment.text;
    REQUIRE_EQ(joined, std::string(" v. giggle ；；idiom. burst out; "));
}

TEST_CASE(translation_gloss_keeps_a_single_sense_whole)
{
    using FanyImeIpc::SplitTranslationGloss;
    // A cloud gloss is one sentence: commas inside it are not sense separators.
    const auto cloud = SplitTranslationGloss("今天天气不错，我们出去走走");
    REQUIRE_EQ(cloud.size(), static_cast<size_t>(1));
    REQUIRE_EQ(cloud[0], std::string("今天天气不错，我们出去走走"));

    REQUIRE(SplitTranslationGloss("").empty());
    REQUIRE(SplitTranslationGloss("   ").empty());
    // Empty senses are dropped rather than becoming blank candidates.
    REQUIRE_EQ(SplitTranslationGloss("；；").size(), static_cast<size_t>(0));
    const auto ragged = SplitTranslationGloss("；apple； ；pear；");
    REQUIRE_EQ(ragged.size(), static_cast<size_t>(2));
    REQUIRE_EQ(ragged[0], std::string("apple"));
    REQUIRE_EQ(ragged[1], std::string("pear"));
}

TEST_CASE(translation_display_hides_every_recognized_part_of_speech_label)
{
    using FanyImeIpc::FormatTranslationGlossForDisplay;
    for (const std::string label : {"proper n.", "interj.", "idiom.", "pron.", "prep.", "conj.", "adj.", "adv.", "num.",
                                    "det.", "aux.", "n.", "v."})
    {
        const std::string raw = label + " \t\r\nmeaning";
        REQUIRE_EQ(FormatTranslationGlossForDisplay(raw, true), raw);
        REQUIRE_EQ(FormatTranslationGlossForDisplay(raw, false), std::string("meaning"));
    }
}

TEST_CASE(translation_display_preserves_separators_whitespace_and_unlabelled_text)
{
    using FanyImeIpc::FormatTranslationGlossForDisplay;
    const std::string raw = " adj. spreading; n. sound of laughter；proper n. Wanshan District；；v. giggle; ";
    REQUIRE_EQ(FormatTranslationGlossForDisplay(raw, true), raw);
    REQUIRE_EQ(FormatTranslationGlossForDisplay(raw, false),
               std::string(" spreading; sound of laughter；Wanshan District；；giggle; "));
    for (const std::string text : {"", " \t\r\n", "n.", "n. ", "very peculiar", "unknown. meaning", "苹果；家伙",
                                   "今天天气不错，我们出去走走", "；；;"})
    {
        REQUIRE_EQ(FormatTranslationGlossForDisplay(text, true), text);
        REQUIRE_EQ(FormatTranslationGlossForDisplay(text, false), text);
    }
}

TEST_CASE(translation_display_toggle_preserves_raw_gloss_and_commit_senses)
{
    using FanyImeIpc::FormatTranslationGlossForDisplay;
    using FanyImeIpc::SplitTranslationGloss;
    std::string raw = "adj. spreading; n. sound of laughter；v. giggle";
    const std::string original = raw;
    const std::vector<std::string> expected{"spreading", "sound of laughter", "giggle"};
    for (const bool show_pos : {true, false, true})
    {
        const auto display = FormatTranslationGlossForDisplay(raw, show_pos);
        REQUIRE_EQ(raw, original);
        REQUIRE(SplitTranslationGloss(raw) == expected);
        REQUIRE(SplitTranslationGloss(display) == expected);
    }
}
