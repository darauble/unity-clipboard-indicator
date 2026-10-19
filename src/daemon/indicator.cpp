#include "indicator.hpp"

#include <cstring>

namespace ci {

namespace {

constexpr const char* kSniPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";

constexpr const char* kSniXml = R"XML(<node>
 <interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconAccessibleDesc" type="s" access="read"/>
  <property name="AttentionIconName" type="s" access="read"/>
  <property name="AttentionAccessibleDesc" type="s" access="read"/>
  <property name="IconThemePath" type="s" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="XAyatanaLabel" type="s" access="read"/>
  <property name="XAyatanaLabelGuide" type="s" access="read"/>
  <property name="XAyatanaOrderingIndex" type="u" access="read"/>
  <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="XAyatanaSecondaryActivate"><arg type="u" direction="in"/></method>
  <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
  <signal name="NewIcon"/>
  <signal name="NewAttentionIcon"/>
  <signal name="NewTitle"/>
  <signal name="NewIconThemePath"><arg type="s"/></signal>
  <signal name="NewStatus"><arg type="s"/></signal>
  <signal name="XAyatanaNewLabel"><arg type="s"/><arg type="s"/></signal>
 </interface>
</node>)XML";

constexpr const char* kMenuXml = R"XML(<node>
 <interface name="com.canonical.dbusmenu">
  <property name="Version" type="u" access="read"/>
  <property name="TextDirection" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconThemePath" type="as" access="read"/>
  <method name="GetLayout">
   <arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
   <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/>
  </method>
  <method name="GetGroupProperties">
   <arg type="ai" direction="in"/><arg type="as" direction="in"/>
   <arg type="a(ia{sv})" direction="out"/>
  </method>
  <method name="GetProperty">
   <arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/>
  </method>
  <method name="Event">
   <arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/>
  </method>
  <method name="EventGroup">
   <arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/>
  </method>
  <method name="AboutToShow">
   <arg type="i" direction="in"/><arg type="b" direction="out"/>
  </method>
  <method name="AboutToShowGroup">
   <arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/>
  </method>
  <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
  <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
  <signal name="ItemActivationRequested"><arg type="i"/><arg type="u"/></signal>
 </interface>
</node>)XML";

GDBusInterfaceInfo* parse_iface(const char* xml) {
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(xml, nullptr);
    GDBusInterfaceInfo* iface = g_dbus_interface_info_ref(node->interfaces[0]);
    g_dbus_node_info_unref(node);
    return iface;
}

std::string escape_mnemonic(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_') out += '_';
        out += c;
    }
    return out;
}

bool wanted(GVariant* filter, const char* name) {
    if (!filter || g_variant_n_children(filter) == 0) return true;
    const char** names = g_variant_get_strv(filter, nullptr);
    bool found = g_strv_contains(names, name);
    g_free(names);
    return found;
}

}  // namespace

Indicator::~Indicator() {
    if (watch_) g_bus_unwatch_name(watch_);
    if (conn_) {
        if (sni_reg_) g_dbus_connection_unregister_object(conn_, sni_reg_);
        if (menu_reg_) g_dbus_connection_unregister_object(conn_, menu_reg_);
    }
}

void Indicator::init(GDBusConnection* conn, std::string icon_theme_path, Callbacks cb) {
    conn_ = conn;
    theme_path_ = std::move(icon_theme_path);
    cb_ = std::move(cb);

    static const GDBusInterfaceVTable sni_vtable{sni_method, sni_property, nullptr, {}};
    static const GDBusInterfaceVTable menu_vtable{menu_method, menu_property, nullptr, {}};
    static GDBusInterfaceInfo* sni_info = parse_iface(kSniXml);
    static GDBusInterfaceInfo* menu_info = parse_iface(kMenuXml);

    sni_reg_ = g_dbus_connection_register_object(conn_, kSniPath, sni_info, &sni_vtable, this, nullptr, nullptr);
    menu_reg_ = g_dbus_connection_register_object(conn_, kMenuPath, menu_info, &menu_vtable, this, nullptr, nullptr);

    // (Re-)register whenever the panel's watcher appears, e.g. after a
    // Unity restart.
    watch_ = g_bus_watch_name_on_connection(
        conn_, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection*, const char*, const char*, gpointer self) {
            static_cast<Indicator*>(self)->register_with_watcher();
        },
        nullptr, this, nullptr);
}

void Indicator::register_with_watcher() {
    g_dbus_connection_call(conn_, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", kSniPath), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           nullptr, nullptr, nullptr);
}

void Indicator::emit_sni(const char* signal, GVariant* args) {
    g_dbus_connection_emit_signal(conn_, nullptr, kSniPath, "org.kde.StatusNotifierItem", signal, args, nullptr);
}

void Indicator::set_icon(const std::string& name) {
    if (name == icon_) return;
    icon_ = name;
    emit_sni("NewIcon", nullptr);
}

void Indicator::set_label(const std::string& label) {
    if (label == label_) return;
    label_ = label;
    emit_sni("XAyatanaNewLabel", g_variant_new("(ss)", label_.c_str(), label_.c_str()));
}

void Indicator::set_visible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    emit_sni("NewStatus", g_variant_new("(s)", visible_ ? "Active" : "Passive"));
}

void Indicator::set_menu(std::vector<MenuItem> items) {
    items_ = std::move(items);
    ++revision_;
    g_dbus_connection_emit_signal(conn_, nullptr, kMenuPath, "com.canonical.dbusmenu", "LayoutUpdated",
                                  g_variant_new("(ui)", revision_, 0), nullptr);
}

const MenuItem* Indicator::find(int id) const {
    for (auto& i : items_)
        if (i.id == id) return &i;
    return nullptr;
}

// ---------------------------------------------------------------------------
// org.kde.StatusNotifierItem

void Indicator::sni_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                           GVariant* params, GDBusMethodInvocation* inv, gpointer user) {
    auto* self = static_cast<Indicator*>(user);
    if (!strcmp(method, "XAyatanaSecondaryActivate")) {
        guint32 t;
        g_variant_get(params, "(u)", &t);
        if (self->cb_.on_secondary_activate) self->cb_.on_secondary_activate(t);
    } else if (!strcmp(method, "SecondaryActivate")) {
        if (self->cb_.on_secondary_activate) self->cb_.on_secondary_activate(0);
    } else if (!strcmp(method, "Scroll")) {
        gint32 delta;
        const char* orientation;
        g_variant_get(params, "(i&s)", &delta, &orientation);
        if (self->cb_.on_scroll && delta) self->cb_.on_scroll(delta);
    }
    g_dbus_method_invocation_return_value(inv, nullptr);
}

GVariant* Indicator::sni_property(GDBusConnection*, const char*, const char*, const char*, const char* name,
                                  GError**, gpointer user) {
    auto* self = static_cast<Indicator*>(user);
    auto str = [](const std::string& s) { return g_variant_new_string(s.c_str()); };
    if (!strcmp(name, "Category")) return g_variant_new_string("ApplicationStatus");
    if (!strcmp(name, "Id")) return g_variant_new_string("clipboard-indicator");
    if (!strcmp(name, "Title")) return g_variant_new_string("Clipboard Indicator");
    if (!strcmp(name, "Status")) return g_variant_new_string(self->visible_ ? "Active" : "Passive");
    if (!strcmp(name, "IconName")) return str(self->icon_);
    if (!strcmp(name, "IconAccessibleDesc")) return g_variant_new_string("Clipboard");
    if (!strcmp(name, "IconThemePath")) return str(self->theme_path_);
    if (!strcmp(name, "Menu")) return g_variant_new_object_path(kMenuPath);
    if (!strcmp(name, "ItemIsMenu")) return g_variant_new_boolean(TRUE);
    if (!strcmp(name, "XAyatanaLabel") || !strcmp(name, "XAyatanaLabelGuide")) return str(self->label_);
    if (!strcmp(name, "XAyatanaOrderingIndex")) return g_variant_new_uint32(0);
    return g_variant_new_string("");
}

// ---------------------------------------------------------------------------
// com.canonical.dbusmenu

GVariant* Indicator::item_props(const MenuItem& item, GVariant* filter) const {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    auto add = [&](const char* key, GVariant* v) {
        if (wanted(filter, key)) g_variant_builder_add(&b, "{sv}", key, v);
        else g_variant_unref(g_variant_ref_sink(v));
    };
    if (item.id == 0) {
        add("children-display", g_variant_new_string("submenu"));
    } else if (item.separator) {
        add("type", g_variant_new_string("separator"));
    } else {
        add("label", g_variant_new_string(escape_mnemonic(item.label).c_str()));
        if (!item.enabled) add("enabled", g_variant_new_boolean(FALSE));
        if (!item.icon_png.empty())
            add("icon-data", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, item.icon_png.data(),
                                                       item.icon_png.size(), 1));
        else if (!item.icon_name.empty())
            add("icon-name", g_variant_new_string(item.icon_name.c_str()));
        if (item.toggle != MenuItem::Toggle::None) {
            add("toggle-type", g_variant_new_string(item.toggle == MenuItem::Toggle::Check ? "checkmark" : "radio"));
            add("toggle-state", g_variant_new_int32(item.checked ? 1 : 0));
        }
        if (!item.shortcut.empty()) {
            GVariantBuilder sb;
            g_variant_builder_init(&sb, G_VARIANT_TYPE("aas"));
            g_variant_builder_open(&sb, G_VARIANT_TYPE("as"));
            for (auto& k : item.shortcut) g_variant_builder_add(&sb, "s", k.c_str());
            g_variant_builder_close(&sb);
            add("shortcut", g_variant_builder_end(&sb));
        }
    }
    return g_variant_builder_end(&b);
}

GVariant* Indicator::layout(int parent, GVariant* filter) const {
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (parent == 0) {
        for (auto& item : items_) {
            GVariantBuilder empty;
            g_variant_builder_init(&empty, G_VARIANT_TYPE("av"));
            g_variant_builder_add(&children, "v",
                                  g_variant_new("(i@a{sv}av)", item.id, item_props(item, filter), &empty));
        }
        static const MenuItem root{};
        return g_variant_new("(i@a{sv}av)", 0, item_props(root, filter), &children);
    }
    const MenuItem* item = find(parent);
    MenuItem missing{};
    missing.id = parent;
    return g_variant_new("(i@a{sv}av)", parent, item_props(item ? *item : missing, filter), &children);
}

void Indicator::menu_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                            GVariant* params, GDBusMethodInvocation* inv, gpointer user) {
    auto* self = static_cast<Indicator*>(user);

    if (!strcmp(method, "GetLayout")) {
        gint32 parent, depth;
        GVariant* filter = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &filter);
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(u@(ia{sv}av))", self->revision_, self->layout(parent, filter)));
        g_variant_unref(filter);
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariant *ids = nullptr, *filter = nullptr;
        g_variant_get(params, "(@ai@as)", &ids, &filter);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        gsize n = 0;
        const gint32* idv = static_cast<const gint32*>(g_variant_get_fixed_array(ids, &n, sizeof(gint32)));
        auto emit = [&](const MenuItem& item) {
            g_variant_builder_add(&b, "(i@a{sv})", item.id, self->item_props(item, filter));
        };
        if (n == 0) {
            for (auto& item : self->items_) emit(item);
        } else {
            for (gsize i = 0; i < n; ++i) {
                if (idv[i] == 0) emit(MenuItem{});
                else if (auto* item = self->find(idv[i])) emit(*item);
            }
        }
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &b));
        g_variant_unref(ids);
        g_variant_unref(filter);
    } else if (!strcmp(method, "GetProperty")) {
        gint32 id;
        const char* name;
        g_variant_get(params, "(i&s)", &id, &name);
        const MenuItem* item = self->find(id);
        GVariant* props = self->item_props(item ? *item : MenuItem{}, nullptr);
        g_variant_ref_sink(props);
        GVariant* v = g_variant_lookup_value(props, name, nullptr);
        g_variant_unref(props);
        if (!v) {
            g_dbus_method_invocation_return_dbus_error(inv, "com.canonical.dbusmenu.Error", "no such property");
            return;
        }
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
        g_variant_unref(v);
    } else if (!strcmp(method, "Event")) {
        gint32 id;
        const char* event;
        GVariant* data;
        guint32 t;
        g_variant_get(params, "(i&svu)", &id, &event, &data, &t);
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, nullptr);
        if (!strcmp(event, "clicked") && self->cb_.on_click) self->cb_.on_click(id, t);
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter* iter;
        g_variant_get(params, "(a(isvu))", &iter);
        gint32 id;
        const char* event;
        GVariant* data;
        guint32 t;
        std::vector<std::pair<int, uint32_t>> clicks;
        while (g_variant_iter_loop(iter, "(i&svu)", &id, &event, &data, &t))
            if (!strcmp(event, "clicked")) clicks.emplace_back(id, t);
        g_variant_iter_free(iter);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        for (auto [cid, ct] : clicks)
            if (self->cb_.on_click) self->cb_.on_click(cid, ct);
    } else if (!strcmp(method, "AboutToShow")) {
        gint32 id;
        g_variant_get(params, "(i)", &id);
        uint32_t before = self->revision_;
        if (id == 0 && self->cb_.on_about_to_show) self->cb_.on_about_to_show();
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", self->revision_ != before));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        GVariant* ids;
        g_variant_get(params, "(@ai)", &ids);
        gsize n = 0;
        const gint32* idv = static_cast<const gint32*>(g_variant_get_fixed_array(ids, &n, sizeof(gint32)));
        uint32_t before = self->revision_;
        for (gsize i = 0; i < n; ++i)
            if (idv[i] == 0 && self->cb_.on_about_to_show) self->cb_.on_about_to_show();
        g_variant_unref(ids);
        GVariantBuilder upd;
        g_variant_builder_init(&upd, G_VARIANT_TYPE("ai"));
        if (self->revision_ != before) g_variant_builder_add(&upd, "i", 0);
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(@ai@ai)", g_variant_builder_end(&upd), g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

GVariant* Indicator::menu_property(GDBusConnection*, const char*, const char*, const char*, const char* name,
                                   GError**, gpointer) {
    if (!strcmp(name, "Version")) return g_variant_new_uint32(3);
    if (!strcmp(name, "TextDirection")) return g_variant_new_string("ltr");
    if (!strcmp(name, "Status")) return g_variant_new_string("normal");
    if (!strcmp(name, "IconThemePath")) return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

}  // namespace ci
