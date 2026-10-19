// Panel indicator for Unity (and other Ayatana/SNI hosts) implemented
// directly on GDBus: org.kde.StatusNotifierItem with the Ayatana label
// extensions plus a com.canonical.dbusmenu server. This avoids pulling GTK
// into the resident daemon, which libappindicator would do.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <gio/gio.h>

namespace ci {

struct MenuItem {
    enum class Toggle { None, Check, Radio };
    int id = 0;
    std::string label;  // plain text; mnemonics are escaped automatically
    std::string icon_name;
    std::string icon_png;  // PNG bytes, takes precedence over icon_name
    bool separator = false;
    bool enabled = true;
    Toggle toggle = Toggle::None;
    bool checked = false;
    std::vector<std::string> shortcut;  // e.g. {"Control", "F9"}
};

class Indicator {
public:
    struct Callbacks {
        std::function<void(int id, uint32_t time)> on_click;
        std::function<void()> on_about_to_show;  // may call set_menu()
        std::function<void(uint32_t time)> on_secondary_activate;  // middle click
        std::function<void(int delta)> on_scroll;
    };

    ~Indicator();
    void init(GDBusConnection* conn, std::string icon_theme_path, Callbacks cb);

    void set_menu(std::vector<MenuItem> items);
    void set_icon(const std::string& name);
    void set_label(const std::string& label);
    void set_visible(bool visible);

private:
    static void sni_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                           GVariant* params, GDBusMethodInvocation* inv, gpointer self);
    static GVariant* sni_property(GDBusConnection*, const char*, const char*, const char*,
                                  const char* name, GError**, gpointer self);
    static void menu_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                            GVariant* params, GDBusMethodInvocation* inv, gpointer self);
    static GVariant* menu_property(GDBusConnection*, const char*, const char*, const char*,
                                   const char* name, GError**, gpointer self);

    void register_with_watcher();
    void emit_sni(const char* signal, GVariant* args);
    GVariant* item_props(const MenuItem& item, GVariant* filter) const;
    GVariant* layout(int parent, GVariant* filter) const;
    const MenuItem* find(int id) const;

    GDBusConnection* conn_ = nullptr;
    guint sni_reg_ = 0, menu_reg_ = 0, watch_ = 0;
    Callbacks cb_;
    std::string theme_path_;
    std::string icon_ = "ci-clipboard-panel";
    std::string label_;
    bool visible_ = true;
    std::vector<MenuItem> items_;
    uint32_t revision_ = 1;
};

}  // namespace ci
