// Preferences window, grouped like the extension's prefs.js. Every change is
// written to settings.ini (debounced); the daemon picks it up via inotify.
#include <string>

#include "common/settings.hpp"
#include "popup.hpp"

namespace ci::ui {

namespace {

GtkWidget* g_window = nullptr;
Settings g_s;
guint g_save_timer = 0;

void save_now() {
    if (g_save_timer) g_source_remove(g_save_timer);
    g_save_timer = 0;
    g_s.save();
}

void schedule_save() {
    if (g_save_timer) g_source_remove(g_save_timer);
    g_save_timer = g_timeout_add(250, [](gpointer) -> gboolean {
        g_save_timer = 0;
        g_s.save();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

GtkWidget* group(GtkWidget* page, const char* title) {
    GtkWidget* l = gtk_label_new(nullptr);
    char* markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_margin_top(l, 12);
    gtk_widget_set_margin_bottom(l, 6);
    gtk_box_pack_start(GTK_BOX(page), l, FALSE, FALSE, 0);

    GtkWidget* frame = gtk_frame_new(nullptr);
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), [](GtkListBoxRow* row, GtkListBoxRow* before, gpointer) {
        if (before && !gtk_list_box_row_get_header(row))
            gtk_list_box_row_set_header(row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    }, nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(frame), list);
    gtk_box_pack_start(GTK_BOX(page), frame, FALSE, FALSE, 0);
    return list;
}

void row(GtkWidget* list, const char* title, const char* subtitle, GtkWidget* control) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_line_wrap(GTK_LABEL(t), TRUE);
    gtk_box_pack_start(GTK_BOX(text), t, FALSE, FALSE, 0);
    if (subtitle) {
        GtkWidget* s = gtk_label_new(subtitle);
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 50);
        gtk_style_context_add_class(gtk_widget_get_style_context(s), "dim-label");
        PangoAttrList* attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
        gtk_label_set_attributes(GTK_LABEL(s), attrs);
        pango_attr_list_unref(attrs);
        gtk_box_pack_start(GTK_BOX(text), s, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(box), control, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(list), box);
}

void add_switch(GtkWidget* list, const char* title, const char* subtitle, bool Settings::*field) {
    GtkWidget* sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), g_s.*field);
    using F = bool Settings::*;
    g_object_set_data_full(G_OBJECT(sw), "ci-field", new F(field), [](gpointer p) { delete static_cast<F*>(p); });
    g_signal_connect(sw, "notify::active", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
        g_s.**static_cast<F*>(g_object_get_data(o, "ci-field")) = gtk_switch_get_active(GTK_SWITCH(o));
        schedule_save();
    }), nullptr);
    row(list, title, subtitle, sw);
}

void add_spin(GtkWidget* list, const char* title, const char* subtitle, int Settings::*field) {
    int min = 0, max = 100000;
    for (auto& k : int_keys())
        if (k.field == field) min = k.min, max = k.max;
    GtkWidget* spin = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), g_s.*field);
    using F = int Settings::*;
    g_object_set_data_full(G_OBJECT(spin), "ci-field", new F(field), [](gpointer p) { delete static_cast<F*>(p); });
    g_signal_connect(spin, "value-changed", G_CALLBACK(+[](GtkSpinButton* s, gpointer) {
        g_s.**static_cast<F*>(g_object_get_data(G_OBJECT(s), "ci-field")) = gtk_spin_button_get_value_as_int(s);
        schedule_save();
    }), nullptr);
    row(list, title, subtitle, spin);
}

std::string accel_label(const std::string& accel) {
    guint key = 0;
    GdkModifierType mods{};
    if (!accel.empty()) gtk_accelerator_parse(accel.c_str(), &key, &mods);
    if (!key) return "Disabled";
    char* l = gtk_accelerator_get_label(key, mods);
    std::string r = l;
    g_free(l);
    return r;
}

void add_shortcut(GtkWidget* list, const char* title, std::string Settings::*field) {
    GtkWidget* b = gtk_button_new_with_label(accel_label(g_s.*field).c_str());
    gtk_widget_set_size_request(b, 160, -1);
    using F = std::string Settings::*;
    g_object_set_data_full(G_OBJECT(b), "ci-field", new F(field), [](gpointer p) { delete static_cast<F*>(p); });
    g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
        g_object_set_data(G_OBJECT(b), "ci-capture", GINT_TO_POINTER(1));
        gtk_button_set_label(b, "Enter shortcut");
        gtk_widget_grab_focus(GTK_WIDGET(b));
    }), nullptr);
    auto stop = +[](GtkWidget* b) {
        g_object_set_data(G_OBJECT(b), "ci-capture", nullptr);
        auto f = *static_cast<F*>(g_object_get_data(G_OBJECT(b), "ci-field"));
        gtk_button_set_label(GTK_BUTTON(b), accel_label(g_s.*f).c_str());
    };
    g_signal_connect(b, "focus-out-event", G_CALLBACK(+[](GtkWidget* b, GdkEvent*, gpointer stop) -> gboolean {
        if (g_object_get_data(G_OBJECT(b), "ci-capture")) reinterpret_cast<void (*)(GtkWidget*)>(stop)(b);
        return FALSE;
    }), reinterpret_cast<gpointer>(stop));
    auto on_key = +[](GtkWidget* b, GdkEventKey* ev, gpointer stop) -> gboolean {
        if (!g_object_get_data(G_OBJECT(b), "ci-capture")) return FALSE;
        if (ev->is_modifier) return TRUE;
        auto f = *static_cast<F*>(g_object_get_data(G_OBJECT(b), "ci-field"));
        guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
        if (ev->keyval == GDK_KEY_Escape && !mods) {
            // cancel
        } else if (ev->keyval == GDK_KEY_BackSpace && !mods) {
            g_s.*f = "";
            schedule_save();
        } else {
            char* name = gtk_accelerator_name(gdk_keyval_to_lower(ev->keyval), GdkModifierType(mods));
            g_s.*f = name;
            g_free(name);
            schedule_save();
        }
        reinterpret_cast<void (*)(GtkWidget*)>(stop)(b);
        return TRUE;
    };
    g_signal_connect(b, "key-press-event", G_CALLBACK(on_key), reinterpret_cast<gpointer>(stop));
    row(list, title, nullptr, b);
}

GtkWidget* page(GtkWidget* stack, const char* name, const char* title) {
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_container_add(GTK_CONTAINER(sw), box);
    gtk_stack_add_titled(GTK_STACK(stack), sw, name, title);
    return box;
}

}  // namespace

void open_settings(GtkApplication* app) {
    if (g_window) {
        gtk_window_present(GTK_WINDOW(g_window));
        return;
    }
    g_s.load();

    g_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_window), "Clipboard Indicator Settings");
    gtk_window_set_icon_name(GTK_WINDOW(g_window), "edit-paste");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 760, 620);
    g_signal_connect(g_window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
        if (g_save_timer) save_now();
        g_window = nullptr;
    }), nullptr);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* stack = gtk_stack_new();
    GtkWidget* sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_widget_set_size_request(sidebar, 160, -1);
    gtk_box_pack_start(GTK_BOX(hbox), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(g_window), hbox);

    GtkWidget* p = page(stack, "ui", "UI");
    GtkWidget* g = group(p, "UI");
    add_switch(g, "Show Search Bar", "Display a search field at the top of the clipboard menu", &Settings::show_search_bar);
    add_switch(g, "Show Private Mode", "Display the private mode toggle in the clipboard menu", &Settings::show_private_mode);
    add_switch(g, "Show Settings Button", "Display a shortcut to these settings in the clipboard menu", &Settings::show_settings_button);
    add_switch(g, "Show Clear History Button", "Display the clear history button in the clipboard menu", &Settings::show_clear_history_button);
    add_switch(g, "Place the pinned section on the bottom", "Move the pinned section to the bottom of the menu", &Settings::pinned_on_bottom);
    g = group(p, "Item Actions");
    add_switch(g, "Delete", "Show the delete button on each item", &Settings::show_delete_button);
    add_switch(g, "Tag", "Show the tag button on each item", &Settings::show_tag_button);
    add_switch(g, "Paste", "Show the paste button on each item", &Settings::paste_button);
    add_switch(g, "Pin", "Show the pin/favorite button on each item", &Settings::show_pin_button);
    add_switch(g, "Edit", "Show the edit button on each text item", &Settings::show_edit_button);
    add_switch(g, "Preview", "Show the preview button on each image item", &Settings::show_preview_button);

    p = page(stack, "behavior", "Behavior");
    g = group(p, "Behavior");
    add_switch(g, "Clear clipboard history on system reboot", "Delete all cached clipboard entries when the system starts", &Settings::clear_on_boot);
    add_switch(g, "Paste on select", "Automatically paste the entry into the active window when selected", &Settings::paste_on_select);
    add_switch(g, "Open menu at cursor", "When using the keyboard shortcut, open the menu at the cursor position", &Settings::open_at_cursor);
    add_switch(g, "Move item to the top after selection", "When selecting an entry, bring it to the top of the history", &Settings::move_item_first);
    add_switch(g, "Keep selected entry after Clear History", "The currently active clipboard entry will not be removed when clearing history", &Settings::keep_selected_on_clear);
    add_switch(g, "Remove whitespace around text", "Strip leading and trailing whitespace from text entries on copy", &Settings::strip_text);
    add_switch(g, "Prompt for confirmation on Clear History", "Ask before deleting all clipboard entries", &Settings::confirm_clear);
    add_switch(g, "Prompt before deleting pinned item", "Ask for confirmation before deleting a pinned item", &Settings::confirm_pinned_delete);
    add_switch(g, "Cache images", "Save copied images to clipboard history", &Settings::cache_images);
    add_switch(g, "Cache only pinned items", "Only save pinned (favorite) entries to disk", &Settings::cache_only_favorites);
    add_switch(g, "Clear clipboard history on interval", "Automatically clear clipboard history at a recurring interval", &Settings::clear_history_on_interval);
    add_spin(g, "History clear interval (in minutes)", nullptr, &Settings::clear_history_interval);

    p = page(stack, "search", "Search");
    g = group(p, "Search");
    add_switch(g, "Case-sensitive", "Match uppercase and lowercase letters exactly when searching", &Settings::case_sensitive_search);
    add_switch(g, "Regular expressions", "Allow regular expressions to filter clipboard entries", &Settings::regex_search);

    p = page(stack, "limits", "Limits");
    g = group(p, "Limits");
    add_spin(g, "History Size", "Maximum number of entries to keep in clipboard history", &Settings::history_size);
    add_spin(g, "Preview Size (characters)", "Number of characters shown per entry in the history menu", &Settings::preview_size);
    add_spin(g, "Max cache file size (MB)", "Maximum disk space used for caching clipboard data", &Settings::cache_size_mb);

    p = page(stack, "exclusion", "Exclusion");
    g = group(p, "Excluded Apps");
    row(g, "Content copied will not be saved while these apps are in focus",
        "One window class (WM_CLASS) per line, e.g. KeePassXC. Find it with: xprop WM_CLASS", nullptr);
    GtkWidget* tv = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 8);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_widget_set_size_request(tv, -1, 140);
    std::string apps;
    for (auto& a : g_s.excluded_apps) apps += a + "\n";
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(buf, apps.c_str(), -1);
    g_signal_connect(buf, "changed", G_CALLBACK(+[](GtkTextBuffer* b, gpointer) {
        GtkTextIter s;
        GtkTextIter e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        char* text = gtk_text_buffer_get_text(b, &s, &e, FALSE);
        g_s.excluded_apps.clear();
        char** lines = g_strsplit(text, "\n", -1);
        for (char** l = lines; *l; ++l)
            if (*g_strstrip(*l)) g_s.excluded_apps.emplace_back(*l);
        g_strfreev(lines);
        g_free(text);
        schedule_save();
    }), nullptr);
    gtk_container_add(GTK_CONTAINER(g), tv);

    p = page(stack, "topbar", "Topbar");
    g = group(p, "Topbar");
    GtkWidget* combo = gtk_combo_box_text_new();
    for (const char* m : {"Icon", "Clipboard Content", "Both", "Neither"})
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), m);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), g_s.display_mode);
    g_signal_connect(combo, "changed", G_CALLBACK(+[](GtkComboBox* c, gpointer) {
        g_s.display_mode = gtk_combo_box_get_active(c);
        schedule_save();
    }), nullptr);
    row(g, "What to show in top bar", "\"Neither\" hides the indicator; use the keyboard shortcut to open the menu", combo);
    add_spin(g, "Number of characters in top bar", "Length of the clipboard content preview shown in the panel", &Settings::topbar_preview_size);
    add_switch(g, "Blink icon on copy", "Briefly flash the indicator icon when something is copied", &Settings::blink_icon_on_copy);

    p = page(stack, "notifications", "Notifications");
    g = group(p, "Notifications");
    add_switch(g, "Show notification on copy", "Display a notification each time text is copied", &Settings::notify_on_copy);
    add_switch(g, "Show notification on cycle", "Display a notification when cycling through entries with shortcuts", &Settings::notify_on_cycle);
    add_switch(g, "Show notification on Clear History", "Display a notification when clipboard history is cleared", &Settings::notify_on_clear);

    p = page(stack, "shortcuts", "Shortcuts");
    g = group(p, "Shortcuts");
    add_switch(g, "Enable shortcuts", "Click a shortcut and press keys to change it; Backspace disables it", &Settings::enable_keybindings);
    add_shortcut(g, "Toggle the menu", &Settings::toggle_menu);
    add_shortcut(g, "Clear history", &Settings::clear_history);
    add_shortcut(g, "Previous entry", &Settings::prev_entry);
    add_shortcut(g, "Next entry", &Settings::next_entry);
    add_shortcut(g, "Private mode", &Settings::private_mode);

    gtk_widget_show_all(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
}

}  // namespace ci::ui
