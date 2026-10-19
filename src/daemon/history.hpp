// Clipboard history: entries in memory (text only; images live on disk) and
// the JSON registry file, compatible with the GNOME extension's format.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ci {

struct Entry {
    uint32_t id = 0;
    bool image = false;
    bool pinned = false;
    std::string mime;
    std::string data;  // text content, or image file path
    std::string tag;

    // Lazily computed menu thumbnail for images (PNG bytes, empty = not yet).
    std::string thumb_png;
    int width = 0, height = 0;
};

class History {
public:
    // Ordered oldest -> newest; the menus show newest first.
    std::vector<Entry> entries;
    uint32_t selected = 0;  // id of the entry currently in the clipboard

    Entry* find(uint32_t id);
    Entry* find_text(std::string_view text);
    Entry* find_image_path(std::string_view path);
    Entry& add(Entry e);
    void remove(uint32_t id, bool delete_file);
    void move_to_end(uint32_t id);
    // Drops the oldest unpinned entries beyond `max`. Returns true if any.
    bool trim(int max);

    // Registry I/O
    void load(const std::string& dir, int max_size, int max_file_mb);
    void save(const std::string& dir, bool only_pinned) const;

    static std::string image_path(const std::string& dir, std::string_view bytes);

private:
    uint32_t next_id_ = 1;
};

std::string json_escape(std::string_view s);

}  // namespace ci
