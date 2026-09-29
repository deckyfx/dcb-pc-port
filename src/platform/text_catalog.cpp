// Text catalog (see text_catalog.hpp).

#include "text_catalog.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace text {

namespace {

/// id -> text from a tsv file (comments and malformed lines skipped, text unescaped).
std::map<std::string, std::string> read_tsv(const std::filesystem::path& path) {
    std::map<std::string, std::string> rows;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        rows[line.substr(0, tab)] = Catalog::unescape(std::string_view(line).substr(tab + 1));
    }
    return rows;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::string Catalog::unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == 'n' || n == 't' || n == '\\') {
                out += n == 'n' ? '\n' : n == 't' ? '\t' : '\\';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

bool Catalog::parse(std::string_view tmpl, std::vector<Token>& out) {
    out.clear();
    std::string literal;
    const auto flush = [&] {
        if (!literal.empty()) out.push_back({Token::Kind::Literal, std::move(literal)});
        literal.clear();
    };
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%') {
            literal += tmpl[i];
            continue;
        }
        size_t j = i + 1;  // flags, width, precision: %3d, %02d, %2.2d, %-4s
        while (j < tmpl.size() && (tmpl[j] == '-' || tmpl[j] == '0' || tmpl[j] == '.' || is_digit(tmpl[j]))) ++j;
        if (j >= tmpl.size()) return false;
        const char conv = tmpl[j];
        if (conv == '%' && j == i + 1) {
            literal += '%';
            i = j;
            continue;
        }
        Token::Kind kind;
        if (conv == 'd') kind = Token::Kind::Int;
        else if (conv == 'c') kind = Token::Kind::Char;
        else if (conv == 's') kind = Token::Kind::Str;
        else return false;
        flush();
        out.push_back({kind, std::string(tmpl.substr(i, j - i + 1))});
        i = j;
    }
    flush();
    return true;
}

bool Catalog::add(std::string id, std::string_view source, std::string_view translation) {
    Entry e;
    e.id = std::move(id);
    if (!parse(source, e.source) || !parse(translation, e.translation)) return false;
    entries_.push_back(std::move(e));
    return true;
}

size_t Catalog::load(const std::filesystem::path& dir, const std::string& lang) {
    const auto source = read_tsv(dir / "source.tsv");
    const auto translated = read_tsv(dir / (lang + ".tsv"));
    size_t added = 0;
    for (const auto& [id, text] : translated) {
        const auto it = source.find(id);
        if (it != source.end() && add(id, it->second, text)) ++added;
    }
    return added;
}

bool Catalog::match(const std::vector<Token>& tokens, size_t t, std::string_view s, size_t pos,
                    std::vector<std::string>& captures) {
    if (t == tokens.size()) return pos == s.size();
    const Token& tok = tokens[t];
    switch (tok.kind) {
    case Token::Kind::Literal:
        if (s.substr(pos, tok.text.size()) != tok.text) return false;
        return match(tokens, t + 1, s, pos + tok.text.size(), captures);
    case Token::Kind::Char:
        if (pos >= s.size()) return false;
        captures.emplace_back(1, s[pos]);
        if (match(tokens, t + 1, s, pos + 1, captures)) return true;
        captures.pop_back();
        return false;
    case Token::Kind::Int: {
        size_t p = pos;
        while (p < s.size() && s[p] == ' ') ++p;  // width padding
        const size_t start = p;
        if (p < s.size() && s[p] == '-') ++p;
        const size_t digits = p;
        while (p < s.size() && is_digit(s[p])) ++p;
        if (p == digits) return false;
        captures.emplace_back(s.substr(start, p - start));
        if (match(tokens, t + 1, s, p, captures)) return true;
        captures.pop_back();
        return false;
    }
    case Token::Kind::Str:
        for (size_t end = pos; end <= s.size(); ++end) {  // shortest first
            captures.emplace_back(s.substr(pos, end - pos));
            if (match(tokens, t + 1, s, end, captures)) return true;
            captures.pop_back();
        }
        return false;
    }
    return false;
}

bool Catalog::translate(std::string_view drawn, std::string& out) const {
    std::vector<std::string> captures;
    for (const Entry& e : entries_) {
        captures.clear();
        // Cheap reject: a template starting with a literal must share its first byte.
        if (!e.source.empty() && e.source[0].kind == Token::Kind::Literal &&
            (drawn.empty() || drawn[0] != e.source[0].text[0]))
            continue;
        if (!match(e.source, 0, drawn, 0, captures)) continue;
        out.clear();
        size_t next = 0;
        for (const Token& tok : e.translation) {
            if (tok.kind == Token::Kind::Literal) {
                out += tok.text;
                continue;
            }
            const std::string value = next < captures.size() ? captures[next] : std::string();
            ++next;
            char buf[64];
            if (tok.kind == Token::Kind::Int)
                std::snprintf(buf, sizeof buf, tok.text.c_str(), std::atoi(value.c_str()));
            else if (tok.kind == Token::Kind::Char)
                std::snprintf(buf, sizeof buf, tok.text.c_str(), value.empty() ? ' ' : value[0]);
            else
                std::snprintf(buf, sizeof buf, tok.text.c_str(), value.c_str());
            out += buf;
        }
        return true;
    }
    return false;
}

}  // namespace text
