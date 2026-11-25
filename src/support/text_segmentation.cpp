#include <mira/text_segmentation.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mira {
namespace {

struct Range final {
    std::uint32_t first;
    std::uint32_t last;
};

enum class CharClass : std::uint8_t { Dropped, Separator, Word, Cjk };

[[nodiscard]] bool within(std::uint32_t codepoint, const Range *ranges,
                          std::size_t count) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (codepoint >= ranges[index].first && codepoint <= ranges[index].last) {
            return true;
        }
    }
    return false;
}

// Characters with no visible or separating role. Dropping them joins what they
// split: "pa<U+200B>ssword" is the word "password" on both sides of a match.
constexpr Range kDropped[]{
    {0x00AD, 0x00AD}, // soft hyphen
    {0x200B, 0x200F}, // zero-width space, non-joiner, joiner, marks
    {0x202A, 0x202E}, // embedding and direction controls
    {0x2060, 0x2064}, // word joiner and invisible operators
    {0xFE00, 0xFE0F}, // variation selectors
    {0xFEFF, 0xFEFF}, // byte order mark
};

// Scripts written without spaces between words: every character is a unit.
constexpr Range kCjk[]{
    {0x3005, 0x3007},   // iteration marks and the ideographic zero
    {0x3031, 0x3035},   // vertical kana repeat marks
    {0x303B, 0x303B},   // vertical iteration mark
    {0x3041, 0x30FF},   // hiragana and katakana
    {0x31F0, 0x31FF},   // katakana phonetic extensions
    {0x3300, 0x33FF},   // CJK compatibility characters
    {0x3400, 0x4DBF},   // Han extension A
    {0x4E00, 0x9FFF},   // Han unified ideographs
    {0xF900, 0xFAFF},   // Han compatibility ideographs
    {0xFF66, 0xFF9F},   // half-width katakana
    {0x20000, 0x2FA1F}, // Han extensions B through F, compatibility supplement
    {0x30000, 0x323AF}, // Han extensions G through I
};

// Punctuation, symbols and whitespace outside ASCII. A character in none of
// these ranges counts as a letter or a digit. A symbol the table misses only
// joins the word around it, and a quoted FTS5 term is tokenized again by
// unicode61, so the miss costs nothing on the durable side.
constexpr Range kSeparators[]{
    {0x0080, 0x00A0}, // C1 controls and no-break space
    {0x00A1, 0x00A9},   {0x00AB, 0x00B1}, {0x00B4, 0x00B4},
    {0x00B6, 0x00B8},   {0x00BB, 0x00BB}, {0x00BF, 0x00BF},
    {0x00D7, 0x00D7},   {0x00F7, 0x00F7}, {0x037E, 0x037E},
    {0x0384, 0x0385},   {0x0387, 0x0387}, {0x0482, 0x0482},
    {0x055A, 0x055F},   {0x0589, 0x058A},                   // Armenian
    {0x05BE, 0x05BE},   {0x05C0, 0x05C0}, {0x05C3, 0x05C3}, // Hebrew
    {0x05C6, 0x05C8},   {0x05F3, 0x05F4}, {0x060C, 0x060D},
    {0x061B, 0x061B},   {0x061F, 0x061F},                   // Arabic
    {0x066A, 0x066D},   {0x06D4, 0x06D4}, {0x0964, 0x0965}, // Devanagari danda
    {0x1680, 0x1680},                                       // Ogham space mark
    {0x2000, 0x206F}, // General Punctuation, including its spaces
    {0x20A0, 0x20CF}, // currency symbols
    {0x2100, 0x214F}, // letterlike symbols
    {0x2190, 0x2BFF}, // arrows, technical and geometric symbols, dingbats
    {0x2E00, 0x2FDF}, // supplemental punctuation, CJK and Kangxi radicals
    {0x3000, 0x3020}, // ideographic space and CJK punctuation and brackets
    {0x3030, 0x3030},   {0x3036, 0x303A}, {0x303C, 0x303C},
    {0x303D, 0x303F},   {0x3040, 0x30FF}, // the few kana-block codepoints that are not kana
    {0x3190, 0x319F},                     // Kanbun
    {0x31C0, 0x31EF},                     // CJK strokes
    {0x3200, 0x32FF},                     // enclosed CJK letters and months
    {0xFF00, 0xFF65},                     // full-width brackets, half-width punctuation
    {0xFFE0, 0xFFE6},   {0xFFE8, 0xFFEE}, {0xFFF9, 0xFFFB},
    {0x1F000, 0x1FBFF}, // Mahjong tiles through symbols and pictographs
};

struct Decoded final {
    std::uint32_t codepoint = 0;
    // Zero for an invalid sequence, which the caller skips one byte at a time.
    std::size_t length = 0;
};

// Strict UTF-8: overlong forms, surrogates and codepoints above U+10FFFF are
// invalid, so a decoded character is always the one its bytes were meant to
// carry.
[[nodiscard]] Decoded decode(std::string_view text, std::size_t offset) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[offset]);
    std::uint32_t codepoint = 0;
    std::size_t length = 0;
    if (lead < 0x80U) {
        codepoint = lead;
        length = 1;
    } else if (lead >= 0xC2U && lead <= 0xDFU) {
        codepoint = lead & 0x1FU;
        length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        codepoint = lead & 0x0FU;
        length = 3;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        codepoint = lead & 0x07U;
        length = 4;
    }
    if (length == 0 || offset + length > text.size()) {
        return {};
    }
    constexpr std::uint32_t kSmallest[] = {0, 0, 0x80, 0x800, 0x10000};
    for (std::size_t index = 1; index < length; ++index) {
        const auto byte = static_cast<std::uint8_t>(text[offset + index]);
        if ((byte & 0xC0U) != 0x80U) {
            return {};
        }
        codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }
    if (codepoint < kSmallest[length] || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return {};
    }
    return {codepoint, length};
}

// Folds full-width ASCII to half-width in place and classifies the result.
[[nodiscard]] CharClass classify(std::uint32_t &codepoint) noexcept {
    if (codepoint >= 0xFF01U && codepoint <= 0xFF5EU) {
        codepoint -= 0xFEE0U;
    }
    if (codepoint < 0x80U) {
        const bool letter =
            (codepoint >= 'A' && codepoint <= 'Z') || (codepoint >= 'a' && codepoint <= 'z');
        const bool digit = codepoint >= '0' && codepoint <= '9';
        return letter || digit || codepoint == '_' ? CharClass::Word : CharClass::Separator;
    }
    if (within(codepoint, kDropped, std::size(kDropped))) {
        return CharClass::Dropped;
    }
    if (within(codepoint, kCjk, std::size(kCjk))) {
        return CharClass::Cjk;
    }
    return within(codepoint, kSeparators, std::size(kSeparators)) ? CharClass::Separator
                                                                  : CharClass::Word;
}

// Hands every unit of `text` to `emit`, in text order, as (kind, text, lone).
// `lone` is set for a character that is the only one in its Han or kana run.
// A unit's view is valid until the next call; a consumer copies what it keeps.
template <typename Emit> void for_each_unit(std::string_view text, Emit &&emit) {
    std::string word;
    std::string pair;
    std::vector<std::pair<std::size_t, std::size_t>> run;
    const auto flush_word = [&]() {
        if (!word.empty()) {
            emit(LexicalUnitKind::Word, word, false);
            word.clear();
        }
    };
    const auto flush_run = [&]() {
        for (std::size_t index = 0; index < run.size(); ++index) {
            const auto first = text.substr(run[index].first, run[index].second);
            emit(LexicalUnitKind::Character, first, run.size() == 1);
            if (index + 1 < run.size()) {
                pair.clear();
                pair.append(first);
                pair.append(text.substr(run[index + 1].first, run[index + 1].second));
                emit(LexicalUnitKind::Bigram, pair, false);
            }
        }
        run.clear();
    };
    for (std::size_t offset = 0; offset < text.size();) {
        const auto decoded = decode(text, offset);
        if (decoded.length == 0) {
            flush_word();
            flush_run();
            ++offset;
            continue;
        }
        auto codepoint = decoded.codepoint;
        switch (classify(codepoint)) {
        case CharClass::Word:
            flush_run();
            if (codepoint < 0x80U) {
                // classify() already folded full-width letters to this range.
                const bool upper = codepoint >= 'A' && codepoint <= 'Z';
                word.push_back(static_cast<char>(upper ? codepoint + 0x20U : codepoint));
            } else {
                word.append(text.substr(offset, decoded.length));
            }
            break;
        case CharClass::Cjk:
            flush_word();
            run.emplace_back(offset, decoded.length);
            break;
        case CharClass::Dropped:
            break;
        case CharClass::Separator:
            flush_word();
            flush_run();
            break;
        }
        offset += decoded.length;
    }
    flush_word();
    flush_run();
}

} // namespace

std::string_view truncate_utf8(std::string_view text, std::size_t limit) noexcept {
    if (limit >= text.size()) {
        return text;
    }
    // Back off while the first excluded byte continues the character the cut
    // would open.
    std::size_t end = limit;
    while (end > 0 && (static_cast<std::uint8_t>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return text.substr(0, end);
}

std::vector<LexicalUnit> segment_lexically(std::string_view text, LexicalUnits units) {
    std::vector<LexicalUnit> result;
    for_each_unit(text, [&](LexicalUnitKind kind, std::string_view unit, bool lone) {
        bool wanted = true;
        switch (kind) {
        case LexicalUnitKind::Bigram:
            wanted = units != LexicalUnits::IndexUnits;
            break;
        case LexicalUnitKind::Character:
            // A lone character is the only coarse unit its run has.
            wanted = units != LexicalUnits::CoarseUnits || lone;
            break;
        case LexicalUnitKind::Word:
            break;
        }
        if (wanted) {
            result.push_back(LexicalUnit{kind, std::string(unit)});
        }
    });
    return result;
}

std::string lexical_index_text(std::string_view text) {
    std::string result;
    for_each_unit(text, [&](LexicalUnitKind kind, std::string_view unit, bool) {
        if (kind == LexicalUnitKind::Bigram) {
            return;
        }
        if (!result.empty()) {
            result.push_back(' ');
        }
        result.append(unit);
    });
    return result;
}

} // namespace mira
