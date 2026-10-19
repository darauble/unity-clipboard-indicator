#include "app.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include "common/text.hpp"

#ifndef CI_DATADIR
#define CI_DATADIR "/usr/local/share/clipboard-indicator"
#endif

namespace ci {

namespace {

enum MenuId : int { kSearch = 1, kPrivate, kSettings, kClear, kEmpty, kSeparator = 10, kEntryBase = 1000 };
enum HotkeyIndex : int { kToggleMenu, kClearHistory, kPrevEntry, kNextEntry, kPrivateMode };

constexpr const char* kApiXml = R"XML(<node>
 <interface name="io.github.UnityClipboardIndicator">
  <method name="GetHistory">
   <arg name="entries" type="a(ubsssbb)" direction="out"/>
   <arg name="private" type="b" direction="out"/>
   <arg name="next_clear" type="x" direction="out"/>
  </method>
  <method name="Activate"><arg name="id" type="u" direction="in"/></method>
  <method name="Paste"><arg name="id" type="u" direction="in"/></method>
  <method name="TogglePin"><arg name="id" type="u" direction="in"/></method>
  <method name="Delete"><arg name="id" type="u" direction="in"/></method>
  <method name="SetTag"><arg name="id" type="u" direction="in"/><arg name="tag" type="s" direction="in"/></method>
  <method name="SetText"><arg name="id" type="u" direction="in"/><arg name="text" type="s" direction="in"/></method>
  <method name="ClearHistory"/>
  <method name="SetPrivate"><arg name="on" type="b" direction="in"/></method>
  <method name="ResetTimer"/>
  <signal name="Changed"/>
 </interface>
</node>)XML";

using Fn = std::function<void()>;

guint later(guint ms, Fn fn) {
    return g_timeout_add_full(
        G_PRIORITY_DEFAULT, ms,
        [](gpointer p) -> gboolean {
            (*static_cast<Fn*>(p))();
            return G_SOURCE_REMOVE;
        },
        new Fn(std::move(fn)), [](gpointer p) { delete static_cast<Fn*>(p); });
}

void cancel(guint& id) {
    if (id) g_source_remove(id);
    id = 0;
}

gint64 now_s() { return g_get_real_time() / G_USEC_PER_SEC; }

std::string read_file(const std::string& path) {
    char* raw = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &raw, &len, nullptr)) return {};
    std::string s(raw, len);
    g_free(raw);
    return s;
}

void write_file(const std::string& path, std::string_view data) {
    g_file_set_contents_full(path.c_str(), data.data(), gssize(data.size()), G_FILE_SET_CONTENTS_CONSISTENT,
                             0600, nullptr);
}

bool is_terminal(const std::string& cls) {
    static constexpr const char* kNames[] = {
        "term", "konsole", "tilix", "alacritty", "kitty", "rxvt", "guake", "tilda", "ghostty",
        "wezterm", "ptyxis", "kgx", "sakura", "foot", "terminator", "terminology", "st-256color",
    };
    return std::ranges::any_of(kNames, [&](const char* n) { return cls.find(n) != std::string::npos; });
}

std::vector<std::string> accel_to_shortcut(const std::string& accel) {
    std::vector<std::string> keys;
    size_t i = 0;
    while (i < accel.size() && accel[i] == '<') {
        size_t end = accel.find('>', i);
        if (end == std::string::npos) return {};
        std::string m = accel.substr(i + 1, end - i - 1);
        if (m == "Primary" || m == "Ctrl") m = "Control";
        keys.push_back(m);
        i = end + 1;
    }
    if (i >= accel.size()) return {};
    keys.push_back(accel.substr(i));
    return keys;
}

}  // namespace

// ---------------------------------------------------------------------------
// Startup

int App::run(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--version")) {
            g_print("clipboard-indicatord " CI_VERSION "\n");
            return 0;
        }
    }

    char* exe = g_file_read_link("/proc/self/exe", nullptr);
    char* dir = g_path_get_dirname(exe ? exe : argv[0]);
    exe_dir_ = dir;
    g_free(dir);
    g_free(exe);

    cache_dir_ = cache_dir();
    g_mkdir_with_parents(cache_dir_.c_str(), 0700);
    s_.load();
    check_boot_clear();
    h_.load(cache_dir_, s_.history_size, s_.cache_size_mb);
    if (auto t = read_file(cache_dir_ + "/next-clear"); !t.empty()) next_clear_ = g_ascii_strtoll(t.c_str(), nullptr, 10);

    GError* err = nullptr;
    conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!conn_) {
        g_printerr("clipboard-indicatord: no session bus: %s\n", err->message);
        return 1;
    }
    // Single instance: DO_NOT_QUEUE (4); reply 1 = primary owner.
    GVariant* reply = g_dbus_connection_call_sync(conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "RequestName",
                                                  g_variant_new("(su)", kBusName, 4u), G_VARIANT_TYPE("(u)"),
                                                  G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    guint32 result = 0;
    if (reply) {
        g_variant_get(reply, "(u)", &result);
        g_variant_unref(reply);
    }
    if (result != 1) {
        g_printerr("clipboard-indicatord: already running\n");
        return 0;
    }

    if (!x_.init({
            .on_content = [this](std::string m, std::string d) { on_content(std::move(m), std::move(d)); },
            .on_owner_gone = [this] { on_owner_gone(); },
            .on_hotkey = [this](int i, Time t) { on_hotkey(i, t); },
        })) {
        g_printerr("clipboard-indicatord: cannot open X display\n");
        return 1;
    }

    std::string icons = exe_dir_ + "/icons";
    if (!g_file_test(icons.c_str(), G_FILE_TEST_IS_DIR)) icons = CI_DATADIR "/icons";

    export_api();
    ind_.init(conn_, icons, {
        .on_click = [this](int id, uint32_t t) {
            switch (id) {
                case kSearch: spawn_ui(nullptr, t); break;
                case kPrivate: set_private(!private_); break;
                case kSettings: spawn_ui("--settings", t); break;
                case kClear: request_clear(t); break;
                default:
                    if (id >= kEntryBase) activate(uint32_t(id - kEntryBase), s_.paste_on_select);
            }
        },
        .on_about_to_show = [this] { rebuild_menu(); },  // refreshes the timer label
        .on_secondary_activate = [this](uint32_t t) { spawn_ui(nullptr, t); },
        .on_scroll = [this](int delta) { cycle(delta > 0 ? -1 : 1); },
    });

    apply_hotkeys();
    setup_interval_clear();

    // Adopt what is in the clipboard now; otherwise restore the newest entry.
    if (x_.has_foreign_owner()) {
        x_.fetch_current();
    } else if (!h_.entries.empty()) {
        select(h_.entries.back().id, true);
    }
    changed(false);

    GFile* f = g_file_new_for_path(Settings::path().c_str());
    g_mkdir_with_parents(Settings::config_dir().c_str(), 0700);
    settings_monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(f);
    if (settings_monitor_) {
        g_signal_connect(settings_monitor_, "changed",
                         G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer p) {
                             auto* self = static_cast<App*>(p);
                             if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && ev != G_FILE_MONITOR_EVENT_CREATED &&
                                 ev != G_FILE_MONITOR_EVENT_DELETED)
                                 return;
                             cancel(self->reload_timer_);
                             self->reload_timer_ = later(150, [self] {
                                 self->reload_timer_ = 0;
                                 self->load_settings();
                             });
                         }),
                         this);
    }

    loop_ = g_main_loop_new(nullptr, FALSE);
    auto quit = +[](gpointer p) -> gboolean {
        g_main_loop_quit(static_cast<App*>(p)->loop_);
        return G_SOURCE_CONTINUE;
    };
    g_unix_signal_add(SIGTERM, quit, this);
    g_unix_signal_add(SIGINT, quit, this);
    g_main_loop_run(loop_);

    if (save_timer_) h_.save(cache_dir_, s_.cache_only_favorites);
    return 0;
}

void App::check_boot_clear() {
    std::string boot = read_file("/proc/sys/kernel/random/boot_id");
    std::string stamp = cache_dir_ + "/boot-id";
    if (boot.empty() || boot == read_file(stamp)) return;
    if (s_.clear_on_boot) {
        if (GDir* d = g_dir_open(cache_dir_.c_str(), 0, nullptr)) {
            while (const char* name = g_dir_read_name(d)) g_unlink((cache_dir_ + "/" + name).c_str());
            g_dir_close(d);
        }
    }
    write_file(stamp, boot);
}

void App::load_settings() {
    Settings old = s_;
    s_.load();
    if (!s_.show_private_mode && private_) set_private(false);
    h_.trim(s_.history_size);
    for (auto& e : h_.entries) e.thumb_png.clear();
    apply_hotkeys();
    if (s_.clear_history_on_interval != old.clear_history_on_interval) setup_interval_clear();
    else if (s_.clear_history_interval != old.clear_history_interval && s_.clear_history_on_interval)
        schedule_next_clear();
    changed(s_.cache_only_favorites != old.cache_only_favorites);
}

void App::apply_hotkeys() {
    std::vector<std::string> accels;
    if (s_.enable_keybindings)
        accels = {s_.toggle_menu, s_.clear_history, s_.prev_entry, s_.next_entry, s_.private_mode};
    std::string state;
    for (auto& a : accels) state += a + '\n';
    if (state == hotkey_state_) return;
    hotkey_state_ = state;
    for (auto& f : x_.set_hotkeys(accels)) g_warning("could not bind shortcut %s (in use?)", f.c_str());
}

// ---------------------------------------------------------------------------
// Clipboard events

bool App::is_excluded() {
    if (s_.excluded_apps.empty()) return false;
    std::string cls = x_.active_window_class();
    if (cls.empty()) return false;
    std::string res_class = cls.substr(0, cls.find('\n'));
    std::string res_name = cls.substr(cls.find('\n') + 1);
    for (auto& app : s_.excluded_apps) {
        char* l = g_ascii_strdown(app.c_str(), -1);
        bool match = res_class == l || res_name == l;
        g_free(l);
        if (match) return true;
    }
    return false;
}

void App::on_content(std::string mime, std::string data) {
    if (private_ || is_excluded()) return;
    bool image = mime.starts_with("image/");
    if (image && !s_.cache_images) return;

    Entry* existing;
    if (image) {
        std::string path = History::image_path(cache_dir_, data);
        existing = h_.find_image_path(path);
        if (!existing) {
            if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) write_file(path, data);
            data = std::move(path);
        }
    } else {
        if (s_.strip_text) data = strip(data);
        if (data.empty()) return;
        existing = h_.find_text(data);
    }

    if (existing) {
        h_.selected = existing->id;
        if (s_.move_item_first && !existing->pinned) h_.move_to_end(existing->id);
        changed();
        return;
    }

    Entry e;
    e.image = image;
    e.mime = std::move(mime);
    e.data = std::move(data);
    h_.selected = h_.add(std::move(e)).id;
    h_.trim(s_.history_size);
    if (s_.notify_on_copy) notify("Copied to clipboard");
    blink();
    changed();
}

void App::on_owner_gone() {
    // The owning app exited: keep its contents available.
    if (private_) return;
    if (Entry* e = h_.find(h_.selected)) own(*e);
}

void App::on_hotkey(int index, Time t) {
    switch (index) {
        case kToggleMenu: spawn_ui(s_.open_at_cursor ? "--at-cursor" : nullptr, uint32_t(t)); break;
        case kClearHistory: request_clear(uint32_t(t)); break;
        case kPrevEntry: cycle(-1); break;
        case kNextEntry: cycle(1); break;
        case kPrivateMode: set_private(!private_); break;
    }
}

// ---------------------------------------------------------------------------
// Actions

void App::own(const Entry& e) {
    if (e.image) x_.own_image(e.mime, e.data);
    else x_.own_text(e.data);
}

void App::clear_clipboard() {
    h_.selected = 0;
    x_.own_text("");
}

void App::select(uint32_t id, bool set_clipboard) {
    Entry* e = h_.find(id);
    if (!e) return;
    h_.selected = id;
    if (set_clipboard) own(*e);
}

void App::activate(uint32_t id, bool paste) {
    Entry* e = h_.find(id);
    if (!e) return;
    select(id, true);
    if (s_.move_item_first && !e->pinned) h_.move_to_end(id);
    changed();
    if (paste) later(150, [this] { x_.send_paste(is_terminal(x_.active_window_class())); });
}

void App::paste_entry(uint32_t id) {
    Entry* e = h_.find(id);
    if (!e) return;
    uint32_t prev = h_.selected;
    suppress_panel_ = true;
    own(*e);
    later(150, [this, id, prev] {
        x_.send_paste(is_terminal(x_.active_window_class()));
        later(250, [this, id, prev] {
            suppress_panel_ = false;
            if (Entry* p = h_.find(prev); p && prev != id) own(*p);
            else h_.selected = id;
            changed(false);
        });
    });
}

void App::remove(uint32_t id) {
    if (id == h_.selected) clear_clipboard();
    h_.remove(id, true);
    changed();
}

void App::toggle_pin(uint32_t id) {
    Entry* e = h_.find(id);
    if (!e) return;
    e->pinned = !e->pinned;
    h_.move_to_end(id);
    changed();
}

void App::clear_history(bool automatic) {
    bool cleared_selected = false;
    std::erase_if(h_.entries, [&](const Entry& e) {
        if (e.pinned || (s_.keep_selected_on_clear && e.id == h_.selected)) return false;
        if (e.image) g_unlink(e.data.c_str());
        if (e.id == h_.selected) cleared_selected = true;
        return true;
    });
    if (cleared_selected) clear_clipboard();
    if (s_.notify_on_clear)
        notify(automatic ? "Clipboard history cleared automatically" : "Clipboard history cleared");
    changed();
}

void App::request_clear(uint32_t time) {
    if (private_) return;
    if (s_.confirm_clear) spawn_ui("--confirm-clear", time);
    else clear_history(false);
}

void App::set_private(bool on) {
    if (on == private_) return;
    private_ = on;
    if (!on) {
        if (Entry* e = h_.find(h_.selected)) own(*e);
        else clear_clipboard();
    }
    changed(false);
}

void App::cycle(int dir) {
    if (private_) return;
    // Same order as the original: history newest first, then pinned.
    std::vector<uint32_t> order;
    for (bool pinned : {false, true})
        for (auto it = h_.entries.rbegin(); it != h_.entries.rend(); ++it)
            if (it->pinned == pinned) order.push_back(it->id);
    if (order.empty()) return;

    int n = int(order.size());
    auto cur = std::ranges::find(order, h_.selected);
    int i = cur == order.end() ? (dir > 0 ? -1 : 0) : int(cur - order.begin());
    i = ((i + dir) % n + n) % n;
    uint32_t id = order[size_t(i)];
    Entry* e = h_.find(id);

    if (s_.notify_on_cycle) {
        std::string text = e->image ? "[Image]" : truncate(e->data, 100);
        notify(std::to_string(i + 1) + " / " + std::to_string(n) + ": " + text);
    }
    cancel(cycle_timer_);
    if (s_.move_item_first) {
        // Show the choice now, commit after a pause so cycling stays stable.
        h_.selected = id;
        changed(false);
        cycle_timer_ = later(750, [this, id] {
            cycle_timer_ = 0;
            if (Entry* e = h_.find(id)) {
                own(*e);
                if (!e->pinned) h_.move_to_end(id);
                changed();
            }
        });
    } else {
        select(id, true);
        changed(false);
    }
}

void App::spawn_ui(const char* arg, uint32_t time) {
    std::string ui = exe_dir_ + "/clipboard-indicator-ui";
    const char* argv[] = {ui.c_str(), arg, nullptr};
    char** env = g_get_environ();
    if (time) {
        std::string id = "clipboard-indicator-" + std::to_string(getpid()) + "_TIME" + std::to_string(time);
        env = g_environ_setenv(env, "DESKTOP_STARTUP_ID", id.c_str(), TRUE);
    }
    GError* err = nullptr;
    if (!g_spawn_async(nullptr, const_cast<char**>(argv), env, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &err)) {
        g_warning("cannot start %s: %s", ui.c_str(), err->message);
        g_error_free(err);
    }
    g_strfreev(env);
}

void App::notify(const std::string& body) {
    if (private_) return;
    char* escaped = g_markup_escape_text(body.c_str(), -1);
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(TRUE));
    g_dbus_connection_call(
        conn_, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susssasa{sv}i)", "Clipboard Indicator", notify_id_, "edit-paste", "Clipboard Indicator",
                      escaped, nullptr, &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer p) {
            if (GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, nullptr)) {
                g_variant_get(r, "(u)", &static_cast<App*>(p)->notify_id_);
                g_variant_unref(r);
            }
        },
        this);
    g_free(escaped);
}

// ---------------------------------------------------------------------------
// Views

void App::changed(bool persist) {
    if (persist) schedule_save();
    rebuild_menu();
    update_panel();
    g_dbus_connection_emit_signal(conn_, nullptr, kObjectPath, kInterface, "Changed", nullptr, nullptr);
}

void App::schedule_save() {
    if (save_timer_) return;
    save_timer_ = later(500, [this] {
        save_timer_ = 0;
        h_.save(cache_dir_, s_.cache_only_favorites);
    });
}

void App::ensure_thumb(Entry& e) {
    if (!e.thumb_png.empty() || e.width < 0) return;
    gdk_pixbuf_get_file_info(e.data.c_str(), &e.width, &e.height);
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(e.data.c_str(), 22, 22, TRUE, nullptr);
    gchar* buf = nullptr;
    gsize len = 0;
    if (pb && gdk_pixbuf_save_to_buffer(pb, &buf, &len, "png", nullptr, nullptr)) {
        e.thumb_png.assign(buf, len);
        g_free(buf);
    } else {
        e.width = -1;  // undecodable; don't retry
    }
    if (pb) g_object_unref(pb);
}

void App::rebuild_menu() {
    std::vector<MenuItem> items;
    int sep = kSeparator;
    auto separator = [&] {
        MenuItem m;
        m.id = sep++;
        m.separator = true;
        items.push_back(std::move(m));
    };

    if (!private_) {
        MenuItem search;
        search.id = kSearch;
        search.label = "Search & manage…";
        search.icon_name = "edit-find";
        if (s_.enable_keybindings) search.shortcut = accel_to_shortcut(s_.toggle_menu);
        items.push_back(std::move(search));
        separator();

        bool first_section = true;
        for (bool pinned : {!s_.pinned_on_bottom, s_.pinned_on_bottom}) {
            bool any = false;
            for (auto it = h_.entries.rbegin(); it != h_.entries.rend(); ++it) {
                if (it->pinned != pinned) continue;
                if (!any && !first_section) separator();
                any = true;
                MenuItem m;
                m.id = kEntryBase + int(it->id);
                if (it->image) {
                    ensure_thumb(*it);
                    m.icon_png = it->thumb_png;
                    m.label = it->width > 0 ? "Image " + std::to_string(it->width) + "×" + std::to_string(it->height)
                                            : "Image";
                } else {
                    m.label = truncate(it->data, s_.preview_size);
                }
                if (!it->tag.empty()) m.label += "   [" + truncate(it->tag, 30) + "]";
                if (it->id == h_.selected) {
                    m.toggle = MenuItem::Toggle::Radio;
                    m.checked = true;
                }
                items.push_back(std::move(m));
            }
            if (any) first_section = false;
        }
        if (h_.entries.empty()) {
            MenuItem empty;
            empty.id = kEmpty;
            empty.label = "Clipboard is empty";
            empty.enabled = false;
            items.push_back(std::move(empty));
        }
        separator();
    }

    if (s_.show_private_mode) {
        MenuItem m;
        m.id = kPrivate;
        m.label = "Private mode";
        m.toggle = MenuItem::Toggle::Check;
        m.checked = private_;
        items.push_back(std::move(m));
    }
    if (s_.show_settings_button) {
        MenuItem m;
        m.id = kSettings;
        m.label = "Settings";
        m.icon_name = "preferences-system";
        items.push_back(std::move(m));
    }
    if (s_.show_clear_history_button && !private_ && !h_.entries.empty()) {
        MenuItem m;
        m.id = kClear;
        m.label = "Clear history";
        if (auto t = timer_text(); !t.empty()) m.label += "  (" + t + ")";
        m.icon_name = "edit-clear";
        items.push_back(std::move(m));
    }
    ind_.set_menu(std::move(items));
}

void App::update_panel() {
    auto mode = DisplayMode(s_.display_mode);
    ind_.set_visible(mode != DisplayMode::Neither);
    if (!blink_timer_) ind_.set_icon(private_ ? "ci-clipboard-panel-private" : "ci-clipboard-panel");
    if (suppress_panel_) return;
    std::string label;
    if (mode == DisplayMode::Content || mode == DisplayMode::Both) {
        const Entry* e = h_.find(h_.selected);
        if (private_ || !e) label = "...";
        else if (e->image) label = "[Image]";
        else label = truncate(e->data, s_.topbar_preview_size);
    }
    ind_.set_label(label);
}

void App::blink() {
    if (!s_.blink_icon_on_copy || private_) return;
    cancel(blink_timer_);
    ind_.set_icon("ci-clipboard-panel-blink");
    blink_timer_ = later(250, [this] {
        blink_timer_ = 0;
        update_panel();
    });
}

// ---------------------------------------------------------------------------
// Interval clearing

void App::setup_interval_clear() {
    cancel(clear_timer_);
    if (!s_.clear_history_on_interval) {
        if (next_clear_ != -1) {
            next_clear_ = -1;
            write_file(cache_dir_ + "/next-clear", "-1");
        }
        return;
    }
    gint64 now = now_s();
    if (next_clear_ == -1) {
        schedule_next_clear();
    } else if (next_clear_ <= now) {
        clear_history(true);
        schedule_next_clear();
    } else {
        clear_timer_ = later(guint(next_clear_ - now) * 1000, [this] {
            clear_timer_ = 0;
            clear_history(true);
            schedule_next_clear();
        });
    }
}

void App::schedule_next_clear() {
    cancel(clear_timer_);
    if (!s_.clear_history_on_interval) return setup_interval_clear();
    next_clear_ = now_s() + gint64(s_.clear_history_interval) * 60;
    write_file(cache_dir_ + "/next-clear", std::to_string(next_clear_));
    clear_timer_ = later(guint(s_.clear_history_interval) * 60 * 1000, [this] {
        clear_timer_ = 0;
        clear_history(true);
        schedule_next_clear();
    });
    changed(false);
}

std::string App::timer_text() const {
    if (!s_.clear_history_on_interval) return {};
    return format_countdown(next_clear_ - now_s());
}

// ---------------------------------------------------------------------------
// D-Bus API

void App::export_api() {
    static GDBusInterfaceInfo* info = [] {
        GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kApiXml, nullptr);
        GDBusInterfaceInfo* i = g_dbus_interface_info_ref(node->interfaces[0]);
        g_dbus_node_info_unref(node);
        return i;
    }();
    static const GDBusInterfaceVTable vtable{api_method, nullptr, nullptr, {}};
    g_dbus_connection_register_object(conn_, kObjectPath, info, &vtable, this, nullptr, nullptr);
}

GVariant* App::history_variant() {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ubsssbb)"));
    if (!private_) {
        for (auto& e : h_.entries) {
            std::string data = valid_utf8(e.data);
            std::string tag = valid_utf8(e.tag);
            g_variant_builder_add(&b, "(ubsssbb)", e.id, e.image, e.mime.c_str(), data.c_str(), tag.c_str(),
                                  e.pinned, e.id == h_.selected);
        }
    }
    gint64 next = s_.clear_history_on_interval ? next_clear_ : -1;
    return g_variant_new("(a(ubsssbb)bx)", &b, private_, next);
}

void App::api_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                     GVariant* params, GDBusMethodInvocation* inv, gpointer p) {
    auto* self = static_cast<App*>(p);
    auto id_arg = [&] {
        guint32 id = 0;
        g_variant_get_child(params, 0, "u", &id);
        return id;
    };

    if (!strcmp(method, "GetHistory")) {
        g_dbus_method_invocation_return_value(inv, self->history_variant());
        return;
    }
    g_dbus_method_invocation_return_value(inv, nullptr);

    if (!strcmp(method, "Activate")) {
        self->activate(id_arg(), self->s_.paste_on_select);
    } else if (!strcmp(method, "Paste")) {
        self->paste_entry(id_arg());
    } else if (!strcmp(method, "TogglePin")) {
        self->toggle_pin(id_arg());
    } else if (!strcmp(method, "Delete")) {
        self->remove(id_arg());
    } else if (!strcmp(method, "SetTag")) {
        guint32 id;
        const char* tag;
        g_variant_get(params, "(u&s)", &id, &tag);
        if (Entry* e = self->h_.find(id)) {
            e->tag = strip(tag);
            self->changed();
        }
    } else if (!strcmp(method, "SetText")) {
        guint32 id;
        const char* text;
        g_variant_get(params, "(u&s)", &id, &text);
        if (Entry* e = self->h_.find(id); e && !e->image) {
            e->data = text;
            if (id == self->h_.selected) self->own(*e);
            self->changed();
        }
    } else if (!strcmp(method, "ClearHistory")) {
        self->clear_history(false);
    } else if (!strcmp(method, "SetPrivate")) {
        gboolean on;
        g_variant_get(params, "(b)", &on);
        self->set_private(on);
    } else if (!strcmp(method, "ResetTimer")) {
        self->schedule_next_clear();
    }
}

}  // namespace ci
