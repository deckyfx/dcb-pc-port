// Text catalog: game strings paired with their translation; port of tools/text/catalog.py.
//
// The catalog (config/SLPS-03101/text/catalog*.txt, embedded) lists ids and offsets only; the text
// comes from the player's dumps. source.tsv holds the JP templates the renderer matches, en.tsv
// the US text or the port's own English (en*.tsv). Template syntax: printf placeholders; the slot
// digit written over "S"/"E" after スロット (US "*S"/"*E") becomes %c, the card count written
// over "??" before 枚 becomes %2d. File escapes: \n, \t, \\.

#include "text_internal.hpp"

#include <algorithm>
#include <cstdio>

namespace patch::text {

namespace {

/// str.splitlines() on UTF-8 text (the line breaks Python knows, ASCII and Unicode).
std::vector<std::string_view> split_lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0, i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t brk = 0;
        if (c == '\r') brk = i + 1 < s.size() && s[i + 1] == '\n' ? 2 : 1;
        else if (c == '\n' || c == '\v' || c == '\f' || (c >= 0x1C && c <= 0x1E)) brk = 1;
        else if (s.substr(i, 2) == "\xC2\x85" || s.substr(i, 3) == "\xE2\x80\xA8" || s.substr(i, 3) == "\xE2\x80\xA9")
            brk = c == 0xC2 ? 2 : 3;
        if (brk) {
            out.push_back(s.substr(start, i - start));
            i += brk;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}

/// Length of the whitespace character at s[i] as str.split() sees it (0: none).
size_t space_at(std::string_view s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F)) return 1;
    if (s.substr(i, 2) == "\xC2\xA0" || s.substr(i, 2) == "\xC2\x85") return 2;
    if (s.substr(i, 3) == "\xE3\x80\x80") return 3;  // ideographic space
    return 0;
}

/// str.split(): tokens separated by whitespace.
std::vector<std::string> split_words(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        if (const size_t n = space_at(s, i)) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
            i += n;
        } else {
            cur += s[i++];
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

struct Line {
    std::string id;
    std::optional<std::string> us;  ///< nullopt: the English comes from the port's en.tsv
    long count = 1;
};

/// catalog.parse: pair / run lines; comments (#) and blank lines skipped.
std::vector<Line> parse(std::string_view text) {
    std::vector<Line> out;
    size_t n = 0;
    for (std::string_view raw : split_lines(text)) {
        ++n;
        const std::vector<std::string> w = split_words(raw.substr(0, raw.find('#')));
        if (w.empty()) continue;
        if (w[0] == "pair" && w.size() == 3) {
            out.push_back({w[1], w[2] == "-" ? std::nullopt : std::optional<std::string>(w[2]), 1});
        } else if (w[0] == "run" && w.size() == 4) {
            size_t used = 0;
            long count = 0;
            try {
                count = std::stol(w[3], &used, 10);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != w[3].size()) throw std::runtime_error("catalog line " + std::to_string(n) + ": bad count");
            out.push_back({w[1], w[2], count});
        } else {
            throw std::runtime_error("catalog line " + std::to_string(n) + ": " + std::string(raw));
        }
    }
    return out;
}

/// catalog.split_id: "FILE:hexoffset".
std::pair<std::string, size_t> split_id(const std::string& id) {
    const size_t colon = id.find(':');
    if (colon == std::string::npos || id.find(':', colon + 1) != std::string::npos)
        throw std::runtime_error("catalog: bad id " + id);
    std::string hex = id.substr(colon + 1);
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex = hex.substr(2);
    size_t off = 0;
    bool any = false;
    for (char c : hex) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == '_' && any) continue;
        else throw std::runtime_error("catalog: bad id " + id);
        off = off * 16 + static_cast<size_t>(v);
        any = true;
    }
    if (!any) throw std::runtime_error("catalog: bad id " + id);
    return {id.substr(0, colon), off};
}

/// catalog.read_string: the NUL-terminated string at `off`, and the offset after its padding.
std::pair<View, size_t> read_string(View blob, size_t off) {
    size_t end = off;
    while (end < blob.size() && blob[end] != 0) ++end;
    if (off > blob.size() || end >= blob.size()) throw std::runtime_error("catalog: no string end");
    size_t next = end + 1;
    while (next < blob.size() && blob[next] == 0) ++next;
    return {blob.subspan(off, end - off), next};
}

std::string hex_id(const std::string& file, size_t off) {
    char buf[24];
    std::snprintf(buf, sizeof buf, ":%zx", off);
    return file + buf;
}

}  // namespace

Bytes jp_template(View raw) {
    // ト (83 67) then the slot letter -> ト%c
    Bytes a;
    for (size_t i = 0; i < raw.size();) {
        if (i + 2 < raw.size() && raw[i] == 0x83 && raw[i + 1] == 0x67 && (raw[i + 2] == 'S' || raw[i + 2] == 'E')) {
            a.insert(a.end(), {0x83, 0x67, '%', 'c'});
            i += 3;
        } else {
            a.push_back(raw[i++]);
        }
    }
    // "??" before 枚 (96 87) -> %2d
    Bytes b;
    for (size_t i = 0; i < a.size();) {
        if (i + 3 < a.size() && a[i] == '?' && a[i + 1] == '?' && a[i + 2] == 0x96 && a[i + 3] == 0x87) {
            b.insert(b.end(), {'%', '2', 'd'});
            i += 2;
        } else {
            b.push_back(a[i++]);
        }
    }
    // The battle banner's player name: "P0" / "P1" after a space and before の (82 CC) -> %s
    Bytes c;
    for (size_t i = 0; i < b.size();) {
        if (i > 0 && i + 3 < b.size() && b[i - 1] == ' ' && b[i] == 'P' && (b[i + 1] == '0' || b[i + 1] == '1') &&
            b[i + 2] == 0x82 && b[i + 3] == 0xCC) {
            c.insert(c.end(), {'%', 's'});
            i += 2;
        } else {
            c.push_back(b[i++]);
        }
    }
    return c;
}

Bytes us_template(View raw) {
    Bytes out;
    for (size_t i = 0; i < raw.size();) {
        if (i + 1 < raw.size() && raw[i] == '*' && (raw[i + 1] == 'S' || raw[i + 1] == 'E')) {
            out.insert(out.end(), {'%', 'c'});
            i += 2;
        } else if (i + 2 < raw.size() && raw[i] == '*' && raw[i + 1] == 'P' && (raw[i + 2] == '0' || raw[i + 2] == '1')) {
            out.insert(out.end(), {'%', 's'});  // the battle banner's player name
            i += 3;
        } else {
            out.push_back(raw[i++]);
        }
    }
    return out;
}

Bytes escape(View text) {
    Bytes out;
    for (uint8_t c : text) {
        if (c == '\\') out.insert(out.end(), {'\\', '\\'});
        else if (c == '\n') out.insert(out.end(), {'\\', 'n'});
        else if (c == '\t') out.insert(out.end(), {'\\', 't'});
        else out.push_back(c);
    }
    return out;
}

void read_own(View tsv, std::map<std::string, Bytes>& own) {
    // bytes.splitlines(): \n, \r, \r\n
    size_t start = 0;
    const auto line = [&](size_t end) {
        const View l = tsv.subspan(start, end - start);
        const bool blank = std::all_of(l.begin(), l.end(), [](uint8_t c) { return c == ' ' || (c >= 9 && c <= 13); });
        if (blank || l[0] == '#') return;
        const auto tab = std::find(l.begin(), l.end(), uint8_t{'\t'});
        const std::string id(l.begin(), tab);
        own[id] = tab == l.end() ? Bytes{} : Bytes(tab + 1, l.end());  // kept in escaped form
    };
    for (size_t i = 0; i < tsv.size();) {
        if (tsv[i] == '\n' || tsv[i] == '\r') {
            line(i);
            i += tsv[i] == '\r' && i + 1 < tsv.size() && tsv[i + 1] == '\n' ? 2 : 1;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < tsv.size()) line(tsv.size());
}

void build_catalog(std::string_view catalog_text, const FileLookup& jp, const FileLookup& us,
                   const std::map<std::string, Bytes>& own, Rows& source, Rows& en,
                   std::vector<std::string>& problems) {
    for (const Line& l : parse(catalog_text)) {
        auto [file, off] = split_id(l.id);
        const View jp_blob = jp(file);
        View us_blob;
        size_t us_off = 0;
        if (l.us) {
            auto [us_name, o] = split_id(*l.us);
            us_blob = us(us_name);
            us_off = o;
        }
        for (long k = 0; k < l.count; ++k) {
            const auto [jp_raw, next_off] = read_string(jp_blob, off);
            const std::string sid = hex_id(file, off);
            source.emplace_back(sid, jp_template(jp_raw));
            std::optional<View> us_raw;
            if (l.us) {  // a run's US side advances even past an own-English entry
                const auto [raw, next] = read_string(us_blob, us_off);
                us_raw = raw;
                us_off = next;
            }
            if (const auto it = own.find(sid); it != own.end()) en.emplace_back(sid, it->second);
            else if (us_raw) en.emplace_back(sid, us_template(*us_raw));
            else problems.push_back(sid + ": no US pair and no entry in the port's en.tsv");
            off = next_off;
        }
    }
}

Bytes write_rows(const Rows& rows, const std::map<std::string, Bytes>* escaped) {
    Bytes out;
    for (const auto& [id, text] : rows) {
        out.insert(out.end(), id.begin(), id.end());
        out.push_back('\t');
        if (escaped && escaped->count(id)) {
            out.insert(out.end(), text.begin(), text.end());
        } else {
            const Bytes e = escape(text);
            out.insert(out.end(), e.begin(), e.end());
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace patch::text
