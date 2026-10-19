#include "history.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>

#include <glib.h>
#include <glib/gstdio.h>

namespace ci {

Entry* History::find(uint32_t id) {
    for (auto& e : entries)
        if (e.id == id) return &e;
    return nullptr;
}

Entry* History::find_text(std::string_view text) {
    for (auto& e : entries)
        if (!e.image && e.data == text) return &e;
    return nullptr;
}

Entry* History::find_image_path(std::string_view path) {
    for (auto& e : entries)
        if (e.image && e.data == path) return &e;
    return nullptr;
}

Entry& History::add(Entry e) {
    e.id = next_id_++;
    return entries.emplace_back(std::move(e));
}

void History::remove(uint32_t id, bool delete_file) {
    auto it = std::ranges::find(entries, id, &Entry::id);
    if (it == entries.end()) return;
    if (delete_file && it->image) g_unlink(it->data.c_str());
    if (selected == id) selected = 0;
    entries.erase(it);
}

void History::move_to_end(uint32_t id) {
    auto it = std::ranges::find(entries, id, &Entry::id);
    if (it == entries.end() || it + 1 == entries.end()) return;
    std::rotate(it, it + 1, entries.end());
}

bool History::trim(int max) {
    int unpinned = int(std::ranges::count(entries, false, &Entry::pinned));
    bool changed = false;
    for (auto it = entries.begin(); unpinned > max && it != entries.end();) {
        if (it->pinned) { ++it; continue; }
        if (it->image) g_unlink(it->data.c_str());
        if (selected == it->id) selected = 0;
        it = entries.erase(it);
        --unpinned;
        changed = true;
    }
    return changed;
}

std::string History::image_path(const std::string& dir, std::string_view bytes) {
    // FNV-1a 64 is plenty to name cache files; collisions only cause dedup.
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
    char name[24];
    std::snprintf(name, sizeof name, "%016llx", (unsigned long long)h);
    return dir + "/" + name;
}

// ---------------------------------------------------------------------------
// Minimal JSON for the registry: an array of flat objects with string/bool
// values. Unknown members (numbers, nested values) are skipped.

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
    return out;
}

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;

    void ws() { while (i < s.size() && g_ascii_isspace(s[i])) ++i; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    static void put_utf8(std::string& out, uint32_t cp) {
        char buf[6];
        int n = g_unichar_to_utf8(cp, buf);
        out.append(buf, n);
    }

    uint32_t hex4() {
        if (i + 4 > s.size()) return 0;
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v = v * 16 + g_ascii_xdigit_value(s[i++]);
        return v;
    }

    std::optional<std::string> str() {
        if (!eat('"')) return std::nullopt;
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            switch (char e = s[i++]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        return std::nullopt;
    }

    // Skips any value; returns false on malformed input.
    bool skip() {
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '"') return str().has_value();
        if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            ++i;
            if (eat(close)) return true;
            do {
                if (close == '}') { if (!str() || !eat(':')) return false; }
                if (!skip()) return false;
            } while (eat(','));
            return eat(close);
        }
        while (i < s.size() && !strchr(",]}", s[i]) && !g_ascii_isspace(s[i])) ++i;
        return true;
    }
};

}  // namespace

void History::load(const std::string& dir, int max_size, int max_file_mb) {
    entries.clear();
    selected = 0;
    std::string path = dir + "/registry.txt";

    GStatBuf st;
    if (g_stat(path.c_str(), &st) != 0) return;
    if (st.st_size >= gint64(max_file_mb) * 1024 * 1024) {
        g_rename(path.c_str(), (path + "~").c_str());
        return;
    }

    char* raw = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &raw, &len, nullptr)) return;
    Parser p{std::string_view(raw, len)};

    if (p.eat('[') && !p.eat(']')) {
        do {
            if (!p.eat('{')) break;
            Entry e;
            e.mime = "text/plain;charset=utf-8";
            bool have_contents = false;
            if (!p.eat('}')) {
                do {
                    auto key = p.str();
                    if (!key || !p.eat(':')) goto done;
                    p.ws();
                    if (*key == "favorite" || *key == "pinned") {
                        e.pinned = p.s.substr(p.i, 4) == "true";
                        p.skip();
                    } else if (*key == "mimetype" || *key == "contents" || *key == "tag") {
                        if (p.i < p.s.size() && p.s[p.i] == '"') {
                            auto v = p.str();
                            if (!v) goto done;
                            if (*key == "mimetype") e.mime = std::move(*v);
                            else if (*key == "tag") e.tag = std::move(*v);
                            else { e.data = std::move(*v); have_contents = true; }
                        } else {
                            p.skip();
                        }
                    } else if (!p.skip()) {
                        goto done;
                    }
                } while (p.eat(','));
                if (!p.eat('}')) break;
            }
            if (!have_contents) continue;
            e.image = e.mime.starts_with("image/");
            if (e.image && !g_file_test(e.data.c_str(), G_FILE_TEST_EXISTS)) continue;
            add(std::move(e));
        } while (p.eat(','));
    }
done:
    g_free(raw);
    trim(max_size);
}

void History::save(const std::string& dir, bool only_pinned) const {
    std::string out = "[";
    bool first = true;
    for (auto& e : entries) {
        if (only_pinned && !e.pinned) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"favorite\":";
        out += e.pinned ? "true" : "false";
        out += ",\"mimetype\":";
        out += json_escape(e.mime);
        out += ",\"contents\":";
        out += json_escape(e.data);
        if (!e.tag.empty()) {
            out += ",\"tag\":";
            out += json_escape(e.tag);
        }
        out += '}';
    }
    out += ']';
    g_mkdir_with_parents(dir.c_str(), 0700);
    std::string path = dir + "/registry.txt";
    if (!g_file_set_contents_full(path.c_str(), out.data(), gssize(out.size()),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr))
        g_warning("failed to write %s", path.c_str());
}

}  // namespace ci
