#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

// ---------------------------------------------------------------------------
// Deterministic text primitives (DEC-147)
//
// Pure functions: no locale, platform, dictionary, model or global state. The
// same bytes always give the same result on every platform, which is what lets
// the in-process lexical legs and the durable FTS5 indexes agree.
// ---------------------------------------------------------------------------

// Longest prefix of `text` that is at most `limit` bytes and ends on a UTF-8
// codepoint boundary, so a bounded copy never ends inside a multi-byte
// character and stays valid UTF-8. A limit at or beyond `text.size()` returns
// `text` unchanged. The function never introduces a cut inside a character;
// text that is already invalid stays as invalid as it was.
[[nodiscard]] std::string_view truncate_utf8(std::string_view text, std::size_t limit) noexcept;

// What one lexical unit is.
enum class LexicalUnitKind : std::uint8_t {
    // A run of letters, digits and underscores from a script that separates
    // words with spaces; ASCII case folded.
    Word,
    // One Han or kana character.
    Character,
    // Two adjacent Han or kana characters, in text order.
    Bigram,
};

// Which units of a text a consumer asks for. The choice depends on the
// consumer, not on the text.
enum class LexicalUnits : std::uint8_t {
    // Words and characters: what a durable FTS5 index stores.
    IndexUnits,
    // `IndexUnits` plus every bigram: what a lexical query is matched with.
    MatchUnits,
    // Words and bigrams; a run of a single character contributes that
    // character. For set and multiset overlap: without inverse-document
    // frequency, single characters are too common to rank with.
    CoarseUnits,
};

struct LexicalUnit final {
    LexicalUnitKind kind = LexicalUnitKind::Word;
    std::string text;
};

// Splits `text` into the requested units in text order. A character and the
// bigram starting at it are both emitted, character first.
//
// Before splitting: full-width ASCII (U+FF01..U+FF5E) becomes half-width,
// zero-width and variation-selector characters are dropped, ASCII letters are
// folded to lower case, and an invalid UTF-8 byte is treated as a separator.
// Every other character separates words unless it is a letter, a digit or a
// similar character. No unit is empty or contains whitespace or a double
// quote, so a unit is always safe inside a quoted FTS5 term.
//
// The case and diacritics of non-ASCII letters are not normalized here;
// SQLite's unicode61 tokenizer applies its own rules on the FTS5 side.
[[nodiscard]] std::vector<LexicalUnit> segment_lexically(std::string_view text, LexicalUnits units);

// The `IndexUnits` of `text` joined by single spaces: what a durable FTS5
// index stores in place of the original text.
[[nodiscard]] std::string lexical_index_text(std::string_view text);

} // namespace mira
