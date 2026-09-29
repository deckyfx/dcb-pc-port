#pragma once
// Text catalog: translates whole game strings by template (tools/text/catalog.py builds the files
// from the player's dumps; config/<serial>/text/catalog.txt says which strings).
//
//   source.tsv   id<TAB>template the game draws (Shift-JIS)
//   <lang>.tsv   id<TAB>translation (en.tsv from the US disc; any language with the same ids)
//
// A template is literal bytes plus printf placeholders the game fills: %d (any width, e.g. %3d),
// %c (one byte: the slot digit the game writes over a letter), %s (any run of bytes), %% (a
// literal %; so is a % that starts no conversion, like the US "30%."). A drawn string that matches a source template whole is replaced by its
// translation, the captured values put into the translation's placeholders in order.
// Escapes in both files: \n line break, \t tab, \\ backslash. Lines starting with # are comments.
//
// No game or SDL dependency: unit-tested on its own (tests/platform/test_text_catalog.cpp).

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace text {

class Catalog {
public:
    /// Adds one entry (templates in raw bytes, already unescaped). A % that starts no
    /// conversion (%d %c %s %%) is a literal %.
    bool add(std::string id, std::string_view source, std::string_view translation);
    /// Loads <dir>/source.tsv and <dir>/<lang>.tsv; ids missing from either are skipped.
    /// Returns the number of entries added.
    size_t load(const std::filesystem::path& dir, const std::string& lang);

    /// The translation of `drawn` when it matches a source template whole.
    bool translate(std::string_view drawn, std::string& out) const;
    /// A message the game types out a character at a time: `drawn` is the start of a source
    /// template (and not a whole one). `out` is the same share of the translation (by Shift-JIS
    /// characters shown), or empty while the start could still be more than one message.
    /// False when no template starts with it, or it holds no Shift-JIS character.
    bool translate_prefix(std::string_view drawn, std::string& out) const;
    size_t size() const { return entries_.size(); }

    /// "\n" -> line break, "\t" -> tab, "\\" -> backslash (the files' escapes).
    static std::string unescape(std::string_view s);

private:
    struct Token {
        enum class Kind { Literal, Int, Char, Str } kind = Kind::Literal;
        std::string text;  ///< Literal: the bytes; placeholders: the printf spec ("%3d")
    };
    struct Entry {
        std::string id;
        std::vector<Token> source, translation;
    };
    static bool parse(std::string_view tmpl, std::vector<Token>& out);
    static bool match(const std::vector<Token>& tokens, size_t t, std::string_view s, size_t pos,
                      std::vector<std::string>& captures);
    /// Like match, but `s` may end anywhere inside the template (true only if it does).
    static bool match_prefix(const std::vector<Token>& tokens, size_t t, std::string_view s, size_t pos,
                             std::vector<std::string>& captures);
    static std::string format(const std::vector<Token>& translation, const std::vector<std::string>& captures);

    std::vector<Entry> entries_;
};

}  // namespace text
