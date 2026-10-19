#include "settings.hpp"

#include <glib.h>

namespace ci {

namespace {

constexpr const char* kGroup = "settings";

constexpr BoolKey kBoolKeys[] = {
    {"show-search-bar", &Settings::show_search_bar},
    {"show-private-mode", &Settings::show_private_mode},
    {"show-settings-button", &Settings::show_settings_button},
    {"show-clear-history-button", &Settings::show_clear_history_button},
    {"pinned-on-bottom", &Settings::pinned_on_bottom},
    {"show-delete-button", &Settings::show_delete_button},
    {"show-tag-button", &Settings::show_tag_button},
    {"paste-button", &Settings::paste_button},
    {"show-pin-button", &Settings::show_pin_button},
    {"show-edit-button", &Settings::show_edit_button},
    {"show-preview-button", &Settings::show_preview_button},
    {"blink-icon-on-copy", &Settings::blink_icon_on_copy},
    {"clear-on-boot", &Settings::clear_on_boot},
    {"paste-on-select", &Settings::paste_on_select},
    {"open-at-cursor", &Settings::open_at_cursor},
    {"move-item-first", &Settings::move_item_first},
    {"keep-selected-on-clear", &Settings::keep_selected_on_clear},
    {"strip-text", &Settings::strip_text},
    {"confirm-clear", &Settings::confirm_clear},
    {"confirm-pinned-delete", &Settings::confirm_pinned_delete},
    {"cache-images", &Settings::cache_images},
    {"cache-only-favorites", &Settings::cache_only_favorites},
    {"clear-history-on-interval", &Settings::clear_history_on_interval},
    {"case-sensitive-search", &Settings::case_sensitive_search},
    {"regex-search", &Settings::regex_search},
    {"notify-on-copy", &Settings::notify_on_copy},
    {"notify-on-cycle", &Settings::notify_on_cycle},
    {"notify-on-clear", &Settings::notify_on_clear},
    {"enable-keybindings", &Settings::enable_keybindings},
};

constexpr IntKey kIntKeys[] = {
    {"history-size", &Settings::history_size, 1, 10000},
    {"preview-size", &Settings::preview_size, 10, 100},
    {"topbar-preview-size", &Settings::topbar_preview_size, 1, 100},
    {"cache-size", &Settings::cache_size_mb, 1, 1024},
    {"clear-history-interval", &Settings::clear_history_interval, 1, 100000},
    {"display-mode", &Settings::display_mode, 0, 3},
};

constexpr StringKey kStringKeys[] = {
    {"toggle-menu", &Settings::toggle_menu},
    {"clear-history", &Settings::clear_history},
    {"prev-entry", &Settings::prev_entry},
    {"next-entry", &Settings::next_entry},
    {"private-mode-binding", &Settings::private_mode},
};

}  // namespace

std::span<const BoolKey> bool_keys() { return kBoolKeys; }
std::span<const IntKey> int_keys() { return kIntKeys; }
std::span<const StringKey> string_keys() { return kStringKeys; }

std::string Settings::config_dir() {
    return std::string(g_get_user_config_dir()) + "/clipboard-indicator";
}

std::string Settings::path() { return config_dir() + "/settings.ini"; }

std::string cache_dir() {
    return std::string(g_get_user_cache_dir()) + "/clipboard-indicator";
}

void Settings::load() {
    *this = Settings{};
    GKeyFile* kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return;
    }
    for (auto& k : kBoolKeys) {
        GError* err = nullptr;
        bool v = g_key_file_get_boolean(kf, kGroup, k.name, &err);
        if (err) g_error_free(err);
        else this->*k.field = v;
    }
    for (auto& k : kIntKeys) {
        GError* err = nullptr;
        int v = g_key_file_get_integer(kf, kGroup, k.name, &err);
        if (err) g_error_free(err);
        else this->*k.field = CLAMP(v, k.min, k.max);
    }
    for (auto& k : kStringKeys) {
        if (char* v = g_key_file_get_string(kf, kGroup, k.name, nullptr)) {
            this->*k.field = v;
            g_free(v);
        }
    }
    gsize n = 0;
    if (char** list = g_key_file_get_string_list(kf, kGroup, "excluded-apps", &n, nullptr)) {
        for (gsize i = 0; i < n; ++i)
            if (*list[i]) excluded_apps.emplace_back(list[i]);
        g_strfreev(list);
    }
    g_key_file_free(kf);
}

bool Settings::save() const {
    g_mkdir_with_parents(config_dir().c_str(), 0700);
    GKeyFile* kf = g_key_file_new();
    for (auto& k : kBoolKeys) g_key_file_set_boolean(kf, kGroup, k.name, this->*k.field);
    for (auto& k : kIntKeys) g_key_file_set_integer(kf, kGroup, k.name, this->*k.field);
    for (auto& k : kStringKeys) g_key_file_set_string(kf, kGroup, k.name, (this->*k.field).c_str());
    std::vector<const char*> apps;
    for (auto& a : excluded_apps) apps.push_back(a.c_str());
    g_key_file_set_string_list(kf, kGroup, "excluded-apps", apps.data(), apps.size());
    bool ok = g_key_file_save_to_file(kf, path().c_str(), nullptr);
    g_key_file_free(kf);
    return ok;
}

}  // namespace ci
