#pragma once

// `english.db` 释义串的格式与合并规则。引擎产出释义、服务端显示并上屏释义，两边必须用同一套
// 规则解释它，所以定义只放这里一处。
//
// 格式：一个键一条释义串，义项之间用分隔符连接 —— en_zh_glosses 存中文释义，用全角「；」；
// zh_en_glosses 存英文释义，用 ASCII 的 "; "。sources/cn-en/ 的机器翻译给英文义项加了词性前缀
// （"n. silver medal"、"proper n. Wanshan District"），候选窗照常显示，Ctrl+Enter 上屏时去掉。
//
// 合并：`custom_translations.txt` 不再整条覆盖内置释义，而是去重后追加。去重先去掉词性前缀，
// 再按大小写不敏感比较，一致即为重复。重复时带词性的写法胜出 —— 词性比大小写更值得保留。

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace translation_gloss
{
// en_zh_glosses 用全角分号（U+FF1B），zh_en_glosses 用 ASCII 的 "; "。
inline constexpr std::string_view kChineseGlossSeparator = "\xEF\xBC\x9B";
inline constexpr std::string_view kEnglishGlossSeparator = "; ";

// 义项开头的词性前缀长度，不含尾随空格："n. " 返回 2，没有词性返回 0。只有标签后面还有内容
// 才算标签，所以孤零零的 "n." 是一段普通文本。
inline std::size_t PartOfSpeechLabelLength(std::string_view sense)
{
    // cn-en 机器翻译使用的词性前缀。长的排在前面："proper n. " 必须先于 "n. " 匹配。
    static constexpr std::string_view kLabels[] = {"proper n. ", "interj. ", "idiom. ", "pron. ", "prep. ", "conj. ",
                                                   "adj. ",      "adv. ",    "num. ",   "det. ",  "aux. ",  "n. ",
                                                   "v. ",        "phr. ",    "int. ",   "part. "};
    for (const auto label : kLabels)
    {
        if (sense.size() > label.size() && sense.substr(0, label.size()) == label)
            return label.size() - 1;
    }
    return 0;
}

inline std::string_view StripPartOfSpeechLabel(std::string_view sense)
{
    const std::size_t label = PartOfSpeechLabelLength(sense);
    if (label == 0)
        return sense;
    const std::size_t rest = sense.find_first_not_of(' ', label);
    return rest == std::string_view::npos ? sense : sense.substr(rest);
}

// 按 ';' 与全角「；」切义项，两端空白去掉，空义项丢弃。返回的片段保留词性前缀，调用方自己决定
// 要不要去掉：合并去重要用键，显示和上屏要分开处理标签。
inline std::vector<std::string_view> SplitSenses(std::string_view gloss)
{
    std::vector<std::string_view> senses;
    const auto push = [&senses](std::string_view sense) {
        const std::size_t first = sense.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return;
        const std::size_t last = sense.find_last_not_of(" \t\r\n");
        senses.push_back(sense.substr(first, last - first + 1));
    };
    std::size_t start = 0;
    while (start <= gloss.size())
    {
        const std::size_t ascii = gloss.find(';', start);
        const std::size_t fullwidth = gloss.find(kChineseGlossSeparator, start);
        const std::size_t cut = (std::min)(ascii, fullwidth);
        if (cut == std::string_view::npos)
        {
            push(gloss.substr(start));
            break;
        }
        push(gloss.substr(start, cut - start));
        start = cut + (cut == fullwidth ? kChineseGlossSeparator.size() : 1);
    }
    return senses;
}

// 去重键：去掉词性、去掉两端空白、ASCII 大写转小写。中文义项不含 ASCII 大写，转换对它们无影响。
inline std::string DedupKey(std::string_view sense)
{
    std::string_view text = StripPartOfSpeechLabel(sense);
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    std::string key(text.substr(first, last - first + 1));
    for (char &ch : key)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return key;
}

// 合并两份释义：base 的义项按原顺序在前，extra 里不重复的追加在后，用 separator 连接。
// 重复时保留带词性的写法；两边都没有词性时保留先出现的。base 和 extra 都为空时返回空串。
inline std::string Merge(std::string_view base, std::string_view extra, std::string_view separator)
{
    std::vector<std::string> kept;
    std::unordered_map<std::string, std::size_t> seen;
    const auto consider = [&kept, &seen](std::string_view sense) {
        std::string key = DedupKey(sense);
        if (key.empty())
            return;
        const auto found = seen.find(key);
        if (found == seen.end())
        {
            seen.emplace(std::move(key), kept.size());
            kept.emplace_back(sense);
            return;
        }
        if (PartOfSpeechLabelLength(kept[found->second]) == 0 && PartOfSpeechLabelLength(sense) != 0)
            kept[found->second] = std::string(sense);
    };
    for (const auto sense : SplitSenses(base))
        consider(sense);
    for (const auto sense : SplitSenses(extra))
        consider(sense);

    std::string merged;
    for (const auto &sense : kept)
    {
        if (!merged.empty())
            merged.append(separator);
        merged.append(sense);
    }
    return merged;
}
} // namespace translation_gloss
