#pragma once

#include <cstdint>
#include <string>

#include <gio/gio.h>

#include "common/settings.hpp"
#include "history.hpp"
#include "indicator.hpp"
#include "x11.hpp"

namespace ci {

class App {
public:
    int run(int argc, char** argv);

private:
    // Clipboard events
    void on_content(std::string mime, std::string data);
    void on_owner_gone();
    void on_hotkey(int index, Time t);

    // Actions (shared by the panel menu, hotkeys and the UI's D-Bus calls)
    void select(uint32_t id, bool set_clipboard);
    void activate(uint32_t id, bool paste);
    void paste_entry(uint32_t id);
    void remove(uint32_t id);
    void toggle_pin(uint32_t id);
    void clear_history(bool automatic);
    void request_clear(uint32_t time);
    void set_private(bool on);
    void cycle(int dir);
    void own(const Entry& e);
    void clear_clipboard();
    void spawn_ui(const char* arg, uint32_t time);
    void notify(const std::string& body);

    // State changes -> views
    void changed(bool persist = true);
    void rebuild_menu();
    void update_panel();
    void blink();
    void schedule_save();
    void load_settings();
    void apply_hotkeys();

    // Interval clearing
    void setup_interval_clear();
    void schedule_next_clear();
    std::string timer_text() const;

    // D-Bus API for the UI
    void export_api();
    static void api_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                           GVariant* params, GDBusMethodInvocation* inv, gpointer self);
    GVariant* history_variant();

    void ensure_thumb(Entry& e);
    bool is_excluded();
    void check_boot_clear();

    Settings s_;
    History h_;
    X11 x_;
    Indicator ind_;
    GMainLoop* loop_ = nullptr;
    GDBusConnection* conn_ = nullptr;
    GFileMonitor* settings_monitor_ = nullptr;
    std::string cache_dir_, exe_dir_;
    bool private_ = false;
    bool suppress_panel_ = false;  // while pasting a non-selected entry
    gint64 next_clear_ = -1;       // epoch seconds
    uint32_t notify_id_ = 0;
    guint save_timer_ = 0, clear_timer_ = 0, blink_timer_ = 0, cycle_timer_ = 0, reload_timer_ = 0;
    std::string hotkey_state_;
};

}  // namespace ci
