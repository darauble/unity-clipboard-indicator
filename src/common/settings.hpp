// Settings shared by the daemon and the UI. Stored as a GKeyFile at
// $XDG_CONFIG_HOME/clipboard-indicator/settings.ini.
#pragma once

#include <span>
#include <string>
#include <vector>

namespace ci {

enum class DisplayMode : int { Icon = 0, Content = 1, Both = 2, Neither = 3 };

struct Settings {
    // Limits
    int history_size = 15;
    int preview_size = 30;
    int topbar_preview_size = 10;
    int cache_size_mb = 5;
    int clear_history_interval = 60;  // minutes
    int display_mode = int(DisplayMode::Icon);

    // UI
    bool show_search_bar = true;
    bool show_private_mode = true;
    bool show_settings_button = true;
    bool show_clear_history_button = true;
    bool pinned_on_bottom = false;
    bool show_delete_button = true;
    bool show_tag_button = true;
    bool paste_button = true;
    bool show_pin_button = true;
    bool show_edit_button = true;
    bool show_preview_button = true;
    bool blink_icon_on_copy = false;

    // Behavior
    bool clear_on_boot = false;
    bool paste_on_select = false;
    bool open_at_cursor = false;
    bool move_item_first = false;
    bool keep_selected_on_clear = false;
    bool strip_text = false;
    bool confirm_clear = true;
    bool confirm_pinned_delete = false;
    bool cache_images = true;
    bool cache_only_favorites = false;
    bool clear_history_on_interval = false;

    // Search
    bool case_sensitive_search = false;
    bool regex_search = false;

    // Notifications
    bool notify_on_copy = false;
    bool notify_on_cycle = true;
    bool notify_on_clear = true;

    // Shortcuts (GTK accelerator syntax, empty = disabled)
    bool enable_keybindings = true;
    std::string toggle_menu = "<Control>F9";
    std::string clear_history = "<Control>F10";
    std::string prev_entry = "<Control>F11";
    std::string next_entry = "<Control>F12";
    std::string private_mode = "<Control>F8";

    // Exclusion (WM_CLASS names)
    std::vector<std::string> excluded_apps;

    void load();
    bool save() const;

    static std::string path();
    static std::string config_dir();
};

// Metadata used by the generic load/save code and the settings window.
struct BoolKey   { const char* name; bool Settings::*field; };
struct IntKey    { const char* name; int Settings::*field; int min, max; };
struct StringKey { const char* name; std::string Settings::*field; };

std::span<const BoolKey> bool_keys();
std::span<const IntKey> int_keys();
std::span<const StringKey> string_keys();

// Cache directory holding the registry and image files.
std::string cache_dir();

// D-Bus API of the daemon used by the UI.
inline constexpr const char* kBusName = "io.github.UnityClipboardIndicator";
inline constexpr const char* kObjectPath = "/io/github/UnityClipboardIndicator";
inline constexpr const char* kInterface = "io.github.UnityClipboardIndicator";
inline constexpr const char* kUiAppId = "io.github.UnityClipboardIndicator.UI";

}  // namespace ci
