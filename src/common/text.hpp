// Small UTF-8 helpers shared by the daemon and the UI.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <glib.h>

namespace ci {

// Collapses whitespace runs to one space and cuts to `max` characters with a
// trailing "...", like the extension's _truncate(). Invalid UTF-8 becomes
// U+FFFD. Only scans as much of `s` as needed, so huge entries stay cheap.
inline std::string truncate(std::string_view s, int max) {
    std::string out;
    int count = 0;
    bool space = false;
    const char* p = s.data();
    const char* end = p + s.size();
    size_t cut = std::string::npos;  // byte length after max-1 chars
    while (p < end) {
        gunichar c = g_utf8_get_char_validated(p, end - p);
        int len;
        if (c == gunichar(-1) || c == gunichar(-2)) {
            c = 0xFFFD;
            len = 1;
        } else {
            len = int(g_utf8_next_char(p) - p);
        }
        p += len;
        if (g_unichar_isspace(c) || c == 0) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            if (count == max - 1) cut = out.size();
            out += ' ';
            if (++count > max) break;
        }
        space = false;
        if (count == max - 1) cut = out.size();
        char buf[6];
        out.append(buf, g_unichar_to_utf8(c, buf));
        if (++count > max) break;
    }
    if (count > max && cut != std::string::npos) {
        out.resize(cut);
        out += "...";
    }
    return out;
}

inline std::string strip(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && g_ascii_isspace(s[b])) ++b;
    while (e > b && g_ascii_isspace(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

inline std::string valid_utf8(std::string_view s) {
    if (g_utf8_validate(s.data(), gssize(s.size()), nullptr) && s.find('\0') == std::string_view::npos)
        return std::string(s);
    char* v = g_utf8_make_valid(s.data(), gssize(s.size()));
    std::string r = v;  // also stops at embedded NULs
    g_free(v);
    return r;
}

// "1h 2m 3s" as in the extension; empty when expired.
inline std::string format_countdown(int64_t left) {
    if (left <= 0) return {};
    std::string r;
    if (left >= 3600) r += std::to_string(left / 3600) + "h ";
    if (left % 3600 >= 60) r += std::to_string(left % 3600 / 60) + "m ";
    r += std::to_string(left % 60) + "s";
    return r;
}

}  // namespace ci
