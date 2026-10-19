#include "popup.hpp"

#include <algorithm>

#include "common/text.hpp"
#include "style.hpp"

namespace ci::ui {

Popup* Popup::current_ = nullptr;

namespace {

constexpr int kShadow = 12;  // transparent border around the frame for its shadow

void add_class(GtkWidget* w, const char* cls) { gtk_style_context_add_class(gtk_widget_get_style_context(w), cls); }

GtkWidget* label(const char* text, const char* cls = nullptr) {
    GtkWidget* l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (cls) add_class(l, cls);
    return l;
}

GtkWidget* separator() {
    GtkWidget* s = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_class(s, "ci-sep");
    return s;
}

// The radio "ornament" of the selected entry: a ring with a dot.
gboolean draw_ornament(GtkWidget* w, cairo_t* cr, gpointer selected) {
    if (!selected) return FALSE;
    double cx = gtk_widget_get_allocated_width(w) / 2.0, cy = gtk_widget_get_allocated_height(w) / 2.0;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 1.5);
    cairo_arc(cr, cx, cy, 6.5, 0, 2 * G_PI);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, 3, 0, 2 * G_PI);
    cairo_fill(cr);
    return FALSE;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifetime

void Popup::toggle(GtkApplication* app, Mode mode, bool at_cursor) {
    if (current_) {
        current_->close();
        return;
    }
    auto* p = new Popup(app, mode, at_cursor);
    if (!p->start()) delete p;
}

Popup::Popup(GtkApplication* app, Mode mode, bool at_cursor) : app_(app), mode_(mode), at_cursor_(at_cursor) {
    current_ = this;
}

bool Popup::start() {
    s_.load();
    conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!conn_ || !fetch()) {
        GtkWidget* d = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                              "The clipboard indicator daemon (clipboard-indicatord) is not running.");
        gtk_window_set_application(GTK_WINDOW(d), app_);
        g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
        gtk_widget_show(d);
        return false;
    }
    changed_sub_ = g_dbus_connection_signal_subscribe(
        conn_, kBusName, kInterface, "Changed", kObjectPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer p) {
            static_cast<Popup*>(p)->on_changed();
        },
        this, nullptr);
    build();
    return true;
}

Popup::~Popup() {
    if (current_ == this) current_ = nullptr;
    if (changed_sub_) g_dbus_connection_signal_unsubscribe(conn_, changed_sub_);
    if (timer_) g_source_remove(timer_);
    if (grab_retry_) g_source_remove(grab_retry_);
    if (regex_) g_regex_unref(regex_);
    if (conn_) g_object_unref(conn_);
}

void Popup::close() {
    ungrab();
    if (preview_) gtk_widget_destroy(preview_);
    preview_ = nullptr;
    if (win_) gtk_widget_destroy(win_);  // "destroy" deletes this
}

bool Popup::fetch() {
    GError* err = nullptr;
    GVariant* r = g_dbus_connection_call_sync(conn_, kBusName, kObjectPath, kInterface, "GetHistory", nullptr,
                                              G_VARIANT_TYPE("(a(ubsssbb)bx)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                              3000, nullptr, &err);
    if (!r) {
        g_printerr("clipboard-indicator-ui: %s\n", err->message);
        g_error_free(err);
        return false;
    }
    GVariantIter* iter;
    gboolean priv;
    g_variant_get(r, "(a(ubsssbb)bx)", &iter, &priv, &next_clear_);
    private_ = priv;
    items_.clear();
    guint32 id;
    gboolean image, pinned, selected;
    const char *mime, *data, *tag;
    while (g_variant_iter_next(iter, "(ub&s&s&sbb)", &id, &image, &mime, &data, &tag, &pinned, &selected))
        items_.push_back({id, bool(image), bool(pinned), bool(selected), mime, data, tag, {}});
    g_variant_iter_free(iter);
    g_variant_unref(r);
    return true;
}

bool Popup::call(const char* method, GVariant* args) {
    GError* err = nullptr;
    GVariant* r = g_dbus_connection_call_sync(conn_, kBusName, kObjectPath, kInterface, method, args, nullptr,
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &err);
    if (!r) {
        g_printerr("clipboard-indicator-ui: %s: %s\n", method, err->message);
        g_error_free(err);
        return false;
    }
    g_variant_unref(r);
    return true;
}

void Popup::on_changed() {
    if (!fetch()) return;
    rebuild_list();
    update_visibility();
    apply_filter();
}

// ---------------------------------------------------------------------------
// Construction

void Popup::build() {
    win_ = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_application(GTK_WINDOW(win_), app_);
    gtk_window_set_type_hint(GTK_WINDOW(win_), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_title(GTK_WINDOW(win_), "Clipboard Indicator");
    gtk_container_set_border_width(GTK_CONTAINER(win_), kShadow);
    add_class(win_, "ci-popup");
    gtk_widget_set_app_paintable(win_, TRUE);
    if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win_)))
        gtk_widget_set_visual(win_, rgba);
    g_signal_connect(win_, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer) -> gboolean {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        return FALSE;
    }), nullptr);
    g_signal_connect(win_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
        auto* self = static_cast<Popup*>(p);
        self->win_ = nullptr;
        delete self;
    }), this);
    g_signal_connect(win_, "key-press-event", G_CALLBACK(on_key), this);
    g_signal_connect(win_, "button-press-event", G_CALLBACK(on_button), this);
    g_signal_connect(win_, "map-event", G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer p) -> gboolean {
        static_cast<Popup*>(p)->grab(w);
        return FALSE;
    }), this);
    g_signal_connect(win_, "grab-broken-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
        auto* self = static_cast<Popup*>(p);
        if (!self->preview_) self->close();  // another client grabbed input
        return TRUE;
    }), this);
    gtk_widget_add_events(win_, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);

    frame_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(frame_, "ci-frame");
    gtk_container_add(GTK_CONTAINER(win_), frame_);

    stack_ = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(stack_), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(stack_), FALSE);
    gtk_box_pack_start(GTK_BOX(frame_), stack_, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(stack_), build_menu_page(), "menu");
    gtk_stack_add_named(GTK_STACK(stack_), build_edit_page(), "edit");
    gtk_stack_add_named(GTK_STACK(stack_), build_tag_page(), "tag");
    gtk_stack_add_named(GTK_STACK(stack_), build_confirm_page(), "confirm");

    rebuild_list();
    gtk_widget_show_all(frame_);
    update_visibility();

    // Remember where the pointer is: the popup opens on that monitor.
    GdkDisplay* display = gdk_display_get_default();
    GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
    gdk_device_get_position(pointer, nullptr, &pointer_x_, &pointer_y_);
    GdkMonitor* monitor = gdk_display_get_monitor_at_point(display, pointer_x_, pointer_y_);
    gdk_monitor_get_workarea(monitor, &workarea_);

    if (mode_ == Mode::ConfirmClear) {
        request_clear();
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "menu");
        if (gtk_widget_get_visible(search_)) gtk_widget_grab_focus(search_);
        else if (GtkWidget* row = row_for_id(0)) gtk_widget_grab_focus(row);
        else if (gtk_widget_get_visible(private_row_)) gtk_widget_grab_focus(private_row_);
    }
    reposition();
    gtk_widget_show(win_);
    update_timer();
}

GtkWidget* Popup::menu_row(const char* icon_name, const char* text, GtkWidget* extra) {
    GtkWidget* btn = gtk_button_new();
    add_class(btn, "ci-row");
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(box), icon(icon_name, 16), FALSE, FALSE, 0);
    GtkWidget* l = label(text);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    if (extra) gtk_box_pack_end(GTK_BOX(box), extra, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(btn), box);
    return btn;
}

GtkWidget* Popup::build_menu_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    search_ = gtk_search_entry_new();
    add_class(search_, "ci-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_), "Type here to search...");
    gtk_box_pack_start(GTK_BOX(page), search_, FALSE, FALSE, 0);
    g_signal_connect(search_, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Popup*>(p)->apply_filter();
    }), this);
    // GtkSearchEntry clears itself on Escape; we want Escape to close.
    g_signal_connect(search_, "stop-search", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Popup*>(p)->close();
    }), this);

    scroller_ = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller_), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller_), 450);
    gtk_box_pack_start(GTK_BOX(page), scroller_, TRUE, TRUE, 0);

    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scroller_), list_);
    gtk_container_set_focus_vadjustment(GTK_CONTAINER(list_),
                                        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)));
    g_signal_connect(list_, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer p) {
        auto* self = static_cast<Popup*>(p);
        if (Item* it = self->item_for_row(GTK_WIDGET(row))) self->activate(Item(*it));
    }), this);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(list_), [](GtkListBoxRow* row, gpointer p) -> gboolean {
        auto* self = static_cast<Popup*>(p);
        Item* it = self->item_for_row(GTK_WIDGET(row));
        return it && self->matches(*it);
    }, this, nullptr);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list_), [](GtkListBoxRow* row, GtkListBoxRow* before, gpointer) {
        bool sep = before && g_object_get_data(G_OBJECT(row), "ci-section") !=
                                 g_object_get_data(G_OBJECT(before), "ci-section");
        if (sep && !gtk_list_box_row_get_header(row)) {
            GtkWidget* s = separator();
            gtk_widget_show(s);
            gtk_list_box_row_set_header(row, s);
        } else if (!sep && gtk_list_box_row_get_header(row)) {
            gtk_list_box_row_set_header(row, nullptr);
        }
    }, nullptr, nullptr);

    empty_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    add_class(empty_, "ci-empty");
    GtkWidget* big = icon("edit-paste-symbolic", 72, "#aaaaaa");
    gtk_widget_set_margin_top(big, 40);
    gtk_box_pack_start(GTK_BOX(empty_), big, FALSE, FALSE, 0);
    GtkWidget* el = gtk_label_new("Clipboard is empty");
    gtk_box_pack_start(GTK_BOX(empty_), el, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), empty_, FALSE, FALSE, 0);

    bottom_sep_ = separator();
    gtk_box_pack_start(GTK_BOX(page), bottom_sep_, FALSE, FALSE, 0);

    private_switch_ = gtk_switch_new();
    gtk_widget_set_valign(private_switch_, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(private_switch_, FALSE);
    g_signal_connect(private_switch_, "notify::active", G_CALLBACK(+[](GObject* sw, GParamSpec*, gpointer p) {
        auto* self = static_cast<Popup*>(p);
        bool on = gtk_switch_get_active(GTK_SWITCH(sw));
        if (!self->updating_ && on != self->private_) self->call("SetPrivate", g_variant_new("(b)", on));
    }), this);
    private_row_ = menu_row("security-medium-symbolic", "Private mode", private_switch_);
    g_signal_connect(private_row_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        auto* self = static_cast<Popup*>(p);
        gtk_switch_set_active(GTK_SWITCH(self->private_switch_), !gtk_switch_get_active(GTK_SWITCH(self->private_switch_)));
    }), this);
    gtk_box_pack_start(GTK_BOX(page), private_row_, FALSE, FALSE, 0);

    settings_row_ = menu_row("preferences-system-symbolic", "Settings", nullptr);
    g_signal_connect(settings_row_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        auto* self = static_cast<Popup*>(p);
        open_settings(self->app_);  // before close() so the app keeps running
        self->close();
    }), this);
    gtk_box_pack_start(GTK_BOX(page), settings_row_, FALSE, FALSE, 0);

    GtkWidget* timer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    timer_label_ = label("", "ci-timer");
    reset_btn_ = gtk_button_new();
    add_class(reset_btn_, "ci-btn");
    gtk_widget_set_can_focus(reset_btn_, FALSE);
    gtk_widget_set_tooltip_text(reset_btn_, "Reset Timer");
    gtk_container_add(GTK_CONTAINER(reset_btn_), icon("view-refresh-symbolic", 14));
    g_signal_connect(reset_btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Popup*>(p)->call("ResetTimer");
    }), this);
    gtk_box_pack_start(GTK_BOX(timer_box), timer_label_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(timer_box), reset_btn_, FALSE, FALSE, 0);
    clear_row_ = menu_row("user-trash-symbolic", "Clear history", timer_box);
    g_signal_connect(clear_row_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Popup*>(p)->request_clear();
    }), this);
    gtk_box_pack_start(GTK_BOX(page), clear_row_, FALSE, FALSE, 0);
    return page;
}

GtkWidget* Popup::dialog_buttons(const char* cancel, const char* ok, void (Popup::*on_ok)(), GtkWidget** ok_out) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
    gtk_widget_set_margin_top(box, 16);
    GtkWidget* c = gtk_button_new_with_label(cancel);
    GtkWidget* o = gtk_button_new_with_label(ok);
    add_class(c, "ci-dialog-btn");
    add_class(o, "ci-dialog-btn");
    gtk_box_pack_start(GTK_BOX(box), c, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), o, TRUE, TRUE, 0);
    g_signal_connect(c, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<Popup*>(p)->cancel_page(); }), this);
    using PageFn = void (Popup::*)();
    g_object_set_data_full(G_OBJECT(o), "ci-fn", new PageFn(on_ok), [](gpointer f) { delete static_cast<PageFn*>(f); });
    g_signal_connect(o, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer p) {
        auto* f = static_cast<PageFn*>(g_object_get_data(G_OBJECT(b), "ci-fn"));
        (static_cast<Popup*>(p)->**f)();
    }), this);
    if (ok_out) *ok_out = o;
    return box;
}

GtkWidget* Popup::build_edit_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    add_class(sw, "ci-edit-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(sw), 400);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 100);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 400);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    edit_view_ = gtk_text_view_new();
    add_class(edit_view_, "ci-edit");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(edit_view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(edit_view_), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(edit_view_), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(edit_view_), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(edit_view_), 8);
    gtk_container_add(GTK_CONTAINER(sw), edit_view_);
    gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), dialog_buttons("Discard", "Save", &Popup::save_edit, &edit_save_), FALSE, FALSE, 0);
    return page;
}

GtkWidget* Popup::build_tag_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(page), 8);
    tag_entry_ = gtk_entry_new();
    add_class(tag_entry_, "ci-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(tag_entry_), "Enter tag…");
    gtk_entry_set_width_chars(GTK_ENTRY(tag_entry_), 34);
    g_signal_connect(tag_entry_, "activate", G_CALLBACK(+[](GtkEntry*, gpointer p) {
        static_cast<Popup*>(p)->save_tag();
    }), this);
    gtk_box_pack_start(GTK_BOX(page), tag_entry_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), dialog_buttons("Discard", "Save", &Popup::save_tag, &tag_save_), FALSE, FALSE, 0);
    return page;
}

GtkWidget* Popup::build_confirm_page() {
    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    confirm_title_ = gtk_label_new("");
    add_class(confirm_title_, "ci-dialog-title");
    confirm_text_ = gtk_label_new("");
    add_class(confirm_text_, "ci-dialog-text");
    gtk_label_set_justify(GTK_LABEL(confirm_text_), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(confirm_text_), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(confirm_text_), 50);
    gtk_box_pack_start(GTK_BOX(page), confirm_title_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), confirm_text_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), dialog_buttons("Cancel", "OK", &Popup::confirm_ok, &confirm_ok_), FALSE, FALSE, 0);
    return page;
}

GtkWidget* Popup::action_button(const char* icon_name, uint32_t id, void (Popup::*fn)(const Item&), bool dim) {
    GtkWidget* b = gtk_button_new();
    add_class(b, "ci-btn");
    if (dim) add_class(b, "ci-dim");
    gtk_widget_set_can_focus(b, FALSE);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(b), icon(icon_name, 16));
    auto* a = new Action{this, id, fn};
    g_signal_connect_data(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        auto* a = static_cast<Action*>(p);
        if (Item* it = a->self->find(a->id)) (a->self->*(a->fn))(Item(*it));
    }), a, [](gpointer p, GClosure*) { delete static_cast<Action*>(p); }, GConnectFlags(0));
    return b;
}

GtkWidget* Popup::make_row(const Item& it, int section) {
    GtkWidget* row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "ci-id", GUINT_TO_POINTER(it.id));
    g_object_set_data(G_OBJECT(row), "ci-section", GINT_TO_POINTER(section + 1));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* orn = gtk_drawing_area_new();
    gtk_widget_set_size_request(orn, 18, 18);
    gtk_widget_set_valign(orn, GTK_ALIGN_CENTER);
    g_signal_connect(orn, "draw", G_CALLBACK(draw_ornament), GINT_TO_POINTER(it.selected));
    gtk_box_pack_start(GTK_BOX(box), orn, FALSE, FALSE, 2);

    if (it.image) {
        GtkWidget* img;
        if (GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(it.data.c_str(), 24, 24, TRUE, nullptr)) {
            img = gtk_image_new_from_pixbuf(pb);
            g_object_unref(pb);
        } else {
            img = icon("image-missing-symbolic", 20);
        }
        add_class(img, "ci-thumb");
        gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    } else {
        GtkWidget* l = label(truncate(it.data, s_.preview_size).c_str());
        gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    }
    if (!it.tag.empty()) {
        GtkWidget* t = gtk_label_new(it.tag.c_str());
        add_class(t, "ci-tag");
        gtk_widget_set_valign(t, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    }

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, 24, -1);
    gtk_box_pack_start(GTK_BOX(box), spacer, TRUE, TRUE, 0);

    if (it.image && s_.show_preview_button)
        gtk_box_pack_start(GTK_BOX(box), action_button("image-x-generic-symbolic", it.id, &Popup::preview, false), FALSE, FALSE, 0);
    if (!it.image && s_.show_edit_button)
        gtk_box_pack_start(GTK_BOX(box), action_button("document-edit-symbolic", it.id, &Popup::edit, false), FALSE, FALSE, 0);
    if (s_.show_pin_button)
        gtk_box_pack_start(GTK_BOX(box), action_button("view-pin-symbolic", it.id, &Popup::toggle_pin, !it.pinned), FALSE, FALSE, 0);
    if (s_.paste_button)
        gtk_box_pack_start(GTK_BOX(box), action_button("edit-paste-symbolic", it.id, &Popup::paste, false), FALSE, FALSE, 0);
    if (s_.show_tag_button)
        gtk_box_pack_start(GTK_BOX(box), action_button("user-bookmarks-symbolic", it.id, &Popup::tag, false), FALSE, FALSE, 0);
    if (s_.show_delete_button)
        gtk_box_pack_start(GTK_BOX(box), action_button("edit-delete-symbolic", it.id, &Popup::remove, false), FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_widget_show_all(row);
    return row;
}

void Popup::rebuild_list() {
    uint32_t focus = focus_after_;
    if (!focus) {
        GtkWidget* f = win_ ? gtk_window_get_focus(GTK_WINDOW(win_)) : nullptr;
        if (f && GTK_IS_LIST_BOX_ROW(f)) focus = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(f), "ci-id"));
    }
    focus_after_ = 0;

    GList* children = gtk_container_get_children(GTK_CONTAINER(list_));
    for (GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    // Pinned section and history section, each newest first.
    int section = 0;
    for (bool pinned : {!s_.pinned_on_bottom, s_.pinned_on_bottom}) {
        bool any = false;
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->pinned != pinned) continue;
            gtk_container_add(GTK_CONTAINER(list_), make_row(*it, section));
            any = true;
        }
        if (any) ++section;
    }
    if (focus)
        if (GtkWidget* row = row_for_id(focus)) gtk_widget_grab_focus(row);
}

void Popup::update_visibility() {
    bool has = !items_.empty();
    gtk_widget_set_visible(search_, s_.show_search_bar && !private_ && has);
    gtk_widget_set_visible(scroller_, !private_ && has);
    gtk_widget_set_visible(empty_, !private_ && !has);
    gtk_widget_set_visible(private_row_, s_.show_private_mode);
    gtk_widget_set_visible(settings_row_, s_.show_settings_button);
    gtk_widget_set_visible(clear_row_, s_.show_clear_history_button && !private_ && has);
    bool any_bottom = s_.show_private_mode || s_.show_settings_button || (s_.show_clear_history_button && has);
    gtk_widget_set_visible(bottom_sep_, !private_ && has && any_bottom);
    bool interval = s_.clear_history_on_interval && next_clear_ > 0;
    gtk_widget_set_visible(timer_label_, interval);
    gtk_widget_set_visible(reset_btn_, interval);
    updating_ = true;
    gtk_switch_set_active(GTK_SWITCH(private_switch_), private_);
    updating_ = false;
    update_timer();
}

void Popup::update_timer() {
    bool interval = s_.clear_history_on_interval && next_clear_ > 0;
    if (!interval) {
        if (timer_) g_source_remove(timer_);
        timer_ = 0;
        return;
    }
    gint64 left = next_clear_ - g_get_real_time() / G_USEC_PER_SEC;
    gtk_label_set_text(GTK_LABEL(timer_label_), format_countdown(left).c_str());
    if (!timer_)
        timer_ = g_timeout_add_seconds(1, [](gpointer p) -> gboolean {
            static_cast<Popup*>(p)->update_timer();
            return G_SOURCE_CONTINUE;
        }, this);
}

// ---------------------------------------------------------------------------
// Filtering, focus helpers

bool Popup::matches(Item& it) {
    if (filter_.empty()) return true;
    static const std::string kImage = "[Image]";
    const std::string& text = it.image ? kImage : it.data;
    if (regex_)
        return g_regex_match(regex_, text.c_str(), GRegexMatchFlags(0), nullptr) ||
               g_regex_match(regex_, it.tag.c_str(), GRegexMatchFlags(0), nullptr);
    if (s_.case_sensitive_search)
        return text.find(filter_) != std::string::npos || it.tag.find(filter_) != std::string::npos;
    if (it.folded.empty()) {
        char* a = g_utf8_casefold(text.c_str(), -1);
        char* b = g_utf8_casefold(it.tag.c_str(), -1);
        it.folded = std::string(a) + '\n' + b;
        g_free(a);
        g_free(b);
    }
    return it.folded.find(filter_) != std::string::npos;
}

void Popup::apply_filter() {
    const char* text = gtk_entry_get_text(GTK_ENTRY(search_));
    if (regex_) g_regex_unref(regex_);
    regex_ = nullptr;
    if (s_.regex_search && *text) {
        auto flags = GRegexCompileFlags(G_REGEX_MULTILINE | (s_.case_sensitive_search ? 0 : G_REGEX_CASELESS));
        regex_ = g_regex_new(text, flags, GRegexMatchFlags(0), nullptr);
        filter_ = regex_ ? text : "";  // invalid pattern: show everything
    } else if (s_.case_sensitive_search || !*text) {
        filter_ = text;
    } else {
        char* f = g_utf8_casefold(text, -1);
        filter_ = f;
        g_free(f);
    }
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(list_));
    reposition();
}

Popup::Item* Popup::find(uint32_t id) {
    for (auto& it : items_)
        if (it.id == id) return &it;
    return nullptr;
}

Popup::Item* Popup::item_for_row(GtkWidget* row) {
    return find(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "ci-id")));
}

// id 0 = first visible row
GtkWidget* Popup::row_for_id(uint32_t id) {
    GtkWidget* found = nullptr;
    GList* children = gtk_container_get_children(GTK_CONTAINER(list_));
    for (GList* l = children; l && !found; l = l->next) {
        auto* row = GTK_WIDGET(l->data);
        if (id ? GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "ci-id")) == id
               : gtk_widget_get_child_visible(row))
            found = row;
    }
    g_list_free(children);
    return found;
}

GtkWidget* Popup::first_visible_row() { return row_for_id(0); }

// Before removing/moving an entry, pick the row that should get focus
// afterwards (next visible row, or the previous one at the end).
void Popup::focus_neighbor_of(uint32_t id) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(list_));
    uint32_t prev = 0, next = 0;
    bool seen = false;
    for (GList* l = children; l; l = l->next) {
        auto* row = GTK_WIDGET(l->data);
        if (!gtk_widget_get_child_visible(row)) continue;
        uint32_t rid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "ci-id"));
        if (rid == id) seen = true;
        else if (!seen) prev = rid;
        else if (!next) next = rid;
    }
    g_list_free(children);
    focus_after_ = next ? next : prev;
}

// ---------------------------------------------------------------------------
// Pages

void Popup::show_page(const char* name, GtkWidget* focus) {
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), name);
    if (focus) gtk_widget_grab_focus(focus);
    reposition();
}

void Popup::cancel_page() {
    if (mode_ == Mode::ConfirmClear) return close();
    show_page("menu", nullptr);
    if (GtkWidget* row = page_item_ ? row_for_id(page_item_) : nullptr) gtk_widget_grab_focus(row);
    else if (gtk_widget_get_visible(search_)) gtk_widget_grab_focus(search_);
    page_item_ = 0;
}

void Popup::confirm(const char* title, const char* text, const char* ok, std::function<void()> action) {
    gtk_label_set_text(GTK_LABEL(confirm_title_), title);
    gtk_label_set_text(GTK_LABEL(confirm_text_), text);
    gtk_button_set_label(GTK_BUTTON(confirm_ok_), ok);
    confirm_action_ = std::move(action);
    show_page("confirm", confirm_ok_);
}

void Popup::confirm_ok() {
    auto action = std::move(confirm_action_);
    confirm_action_ = nullptr;
    if (action) action();
    cancel_page();
}

void Popup::request_clear() {
    if (!s_.confirm_clear && mode_ == Mode::Menu) {
        call("ClearHistory");
        return;
    }
    confirm("Clear all?", "Are you sure you want to delete all clipboard items?\nThis operation cannot be undone.",
            "Clear", [this] { call("ClearHistory"); });
}

void Popup::edit(const Item& it) {
    page_item_ = it.id;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(edit_view_)), it.data.c_str(), -1);
    show_page("edit", edit_view_);
}

void Popup::save_edit() {
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(edit_view_));
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    char* text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    call("SetText", g_variant_new("(us)", page_item_, text));
    g_free(text);
    cancel_page();
}

void Popup::tag(const Item& it) {
    page_item_ = it.id;
    gtk_entry_set_text(GTK_ENTRY(tag_entry_), it.tag.c_str());
    show_page("tag", tag_entry_);
}

void Popup::save_tag() {
    call("SetTag", g_variant_new("(us)", page_item_, gtk_entry_get_text(GTK_ENTRY(tag_entry_))));
    cancel_page();
}

// ---------------------------------------------------------------------------
// Item actions

void Popup::activate(const Item& it) {
    // Release input first so a paste-on-select keystroke reaches the app.
    ungrab();
    gtk_widget_hide(win_);
    gdk_display_flush(gdk_display_get_default());
    call("Activate", g_variant_new("(u)", it.id));
    close();
}

void Popup::paste(const Item& it) {
    ungrab();
    gtk_widget_hide(win_);
    gdk_display_flush(gdk_display_get_default());
    call("Paste", g_variant_new("(u)", it.id));
    close();
}

void Popup::toggle_pin(const Item& it) {
    focus_after_ = it.id;
    call("TogglePin", g_variant_new("(u)", it.id));
}

void Popup::remove(const Item& it) {
    auto doit = [this, id = it.id] {
        focus_neighbor_of(id);
        call("Delete", g_variant_new("(u)", id));
    };
    if (it.pinned && s_.confirm_pinned_delete) {
        page_item_ = 0;
        confirm("Delete pinned item?", "Are you sure you want to delete this pinned item?\nThis operation cannot be undone.",
                "Delete", doit);
    } else {
        doit();
    }
}

void Popup::preview(const Item& it) {
    close_preview();
    GdkRectangle mon;
    GdkDisplay* display = gdk_display_get_default();
    gdk_monitor_get_geometry(gdk_display_get_monitor_at_point(display, pointer_x_, pointer_y_), &mon);

    // Fit into half the width and 40% of the height, never upscaling.
    int max_w = mon.width / 2, max_h = mon.height * 2 / 5, ow = 0, oh = 0;
    gdk_pixbuf_get_file_info(it.data.c_str(), &ow, &oh);
    GdkPixbuf* pb = ow > 0 && ow <= max_w && oh <= max_h
                        ? gdk_pixbuf_new_from_file(it.data.c_str(), nullptr)
                        : gdk_pixbuf_new_from_file_at_scale(it.data.c_str(), max_w, max_h, TRUE, nullptr);
    if (!pb) return;

    preview_ = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_application(GTK_WINDOW(preview_), app_);
    add_class(preview_, "ci-overlay");
    gtk_widget_set_app_paintable(preview_, TRUE);
    if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(preview_)))
        gtk_widget_set_visual(preview_, rgba);
    g_signal_connect(preview_, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer) -> gboolean {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
        cairo_paint(cr);
        return FALSE;
    }), nullptr);
    gtk_window_move(GTK_WINDOW(preview_), mon.x, mon.y);
    gtk_window_resize(GTK_WINDOW(preview_), mon.width, mon.height);
    GtkWidget* img = gtk_image_new_from_pixbuf(pb);
    g_object_unref(pb);
    gtk_container_add(GTK_CONTAINER(preview_), img);
    gtk_widget_add_events(preview_, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    auto close_cb = +[](GtkWidget*, GdkEvent* ev, gpointer p) -> gboolean {
        if (ev->type == GDK_KEY_PRESS && ev->key.keyval != GDK_KEY_Escape && ev->key.keyval != GDK_KEY_h &&
            ev->key.keyval != GDK_KEY_space && ev->key.keyval != GDK_KEY_Return)
            return TRUE;
        static_cast<Popup*>(p)->close_preview();
        return TRUE;
    };
    g_signal_connect(preview_, "button-press-event", G_CALLBACK(close_cb), this);
    g_signal_connect(preview_, "key-press-event", G_CALLBACK(close_cb), this);
    g_signal_connect(preview_, "map-event", G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer p) -> gboolean {
        static_cast<Popup*>(p)->grab(w);
        return FALSE;
    }), this);
    gtk_widget_show_all(preview_);
}

void Popup::close_preview() {
    if (!preview_) return;
    GtkWidget* p = preview_;
    preview_ = nullptr;
    ungrab();
    gtk_widget_destroy(p);
    if (win_) grab(win_);
}

// ---------------------------------------------------------------------------
// Window placement, grabs, input

void Popup::reposition() {
    if (!win_) return;
    GtkRequisition nat;
    gtk_widget_get_preferred_size(win_, nullptr, &nat);
    const GdkRectangle& wa = workarea_;
    int w = nat.width, h = nat.height;
    int x, y;
    if (mode_ == Mode::ConfirmClear) {
        x = wa.x + (wa.width - w) / 2;
        y = wa.y + (wa.height - h) / 3;
    } else if (at_cursor_) {
        x = pointer_x_ - kShadow;
        y = pointer_y_ - kShadow;
    } else {
        // Drop down from the panel at the right, like a panel menu.
        x = wa.x + wa.width - w + kShadow / 2;
        y = wa.y - kShadow + 4;
    }
    x = std::clamp(x, wa.x - kShadow, std::max(wa.x - kShadow, wa.x + wa.width - w + kShadow));
    y = std::clamp(y, wa.y - kShadow, std::max(wa.y - kShadow, wa.y + wa.height - h + kShadow));
    gtk_window_move(GTK_WINDOW(win_), x, y);
    gtk_window_resize(GTK_WINDOW(win_), w, h);
}

void Popup::grab(GtkWidget* w) {
    if (grab_retry_) g_source_remove(grab_retry_);
    grab_retry_ = 0;
    GdkWindow* gw = gtk_widget_get_window(w);
    if (!gw) return;
    GdkSeat* seat = gdk_display_get_default_seat(gtk_widget_get_display(w));
    if (gdk_seat_grab(seat, gw, GDK_SEAT_CAPABILITY_ALL, TRUE, nullptr, nullptr, nullptr, nullptr) == GDK_GRAB_SUCCESS) {
        grab_tries_ = 0;
        // Make the window behave as focused so entries show a cursor (the
        // same trick GtkTreeView uses for its search popup).
        GdkEvent* fe = gdk_event_new(GDK_FOCUS_CHANGE);
        fe->focus_change.window = GDK_WINDOW(g_object_ref(gw));
        fe->focus_change.send_event = TRUE;
        fe->focus_change.in = TRUE;
        gdk_event_set_device(fe, gdk_seat_get_keyboard(seat));
        gtk_widget_send_focus_change(w, fe);
        gdk_event_free(fe);
        return;
    }
    // The hotkey's own grab may still be active for a moment; retry.
    if (++grab_tries_ > 40) return;
    struct Retry { Popup* self; GtkWidget* w; };
    grab_retry_ = g_timeout_add_full(G_PRIORITY_DEFAULT, 25, [](gpointer p) -> gboolean {
        auto* r = static_cast<Retry*>(p);
        r->self->grab_retry_ = 0;
        r->self->grab(r->w);
        return G_SOURCE_REMOVE;
    }, new Retry{this, w}, [](gpointer p) { delete static_cast<Retry*>(p); });
}

void Popup::ungrab() {
    if (grab_retry_) g_source_remove(grab_retry_);
    grab_retry_ = 0;
    gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));
}

gboolean Popup::on_button(GtkWidget* w, GdkEventButton* ev, gpointer p) {
    auto* self = static_cast<Popup*>(p);
    int ox, oy;
    gdk_window_get_origin(gtk_widget_get_window(w), &ox, &oy);
    GtkAllocation a;
    gtk_widget_get_allocation(self->frame_, &a);
    double x = ev->x_root - ox, y = ev->y_root - oy;
    if (x < a.x || y < a.y || x >= a.x + a.width || y >= a.y + a.height) {
        self->close();  // click outside the menu
        return TRUE;
    }
    return FALSE;
}

gboolean Popup::on_key(GtkWidget* w, GdkEventKey* ev, gpointer p) {
    auto* self = static_cast<Popup*>(p);
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    const char* page = gtk_stack_get_visible_child_name(GTK_STACK(self->stack_));

    if (g_strcmp0(page, "menu") != 0) {
        if (ev->keyval == GDK_KEY_Escape) {
            self->cancel_page();
            return TRUE;
        }
        if (!g_strcmp0(page, "edit") && (mods & GDK_CONTROL_MASK) &&
            (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter)) {
            self->save_edit();
            return TRUE;
        }
        return FALSE;
    }

    if (ev->keyval == GDK_KEY_Escape) {
        self->close();
        return TRUE;
    }
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(w));
    if (focus == self->search_) {
        if (ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_KP_Down) {
            if (GtkWidget* row = self->first_visible_row()) gtk_widget_grab_focus(row);
            return TRUE;
        }
        if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
            if (GtkWidget* row = self->first_visible_row())
                if (Item* it = self->item_for_row(row)) self->activate(Item(*it));
            return TRUE;
        }
        return FALSE;
    }
    if (focus && GTK_IS_LIST_BOX_ROW(focus)) {
        Item* found = self->item_for_row(focus);
        if (!found) return FALSE;
        Item it = *found;
        if (!mods) {
            switch (ev->keyval) {
                case GDK_KEY_Delete:
                case GDK_KEY_KP_Delete: self->remove(it); return TRUE;
                case GDK_KEY_Return:
                case GDK_KEY_KP_Enter:
                case GDK_KEY_ISO_Enter:
                    // GtkListBox's own binding needs its cursor row, which
                    // is lost when rows are rebuilt.
                    self->activate(it);
                    return TRUE;
                case GDK_KEY_p: self->toggle_pin(it); return TRUE;
                case GDK_KEY_v: self->paste(it); return TRUE;
                case GDK_KEY_t: self->tag(it); return TRUE;
                case GDK_KEY_h:
                    if (it.image) self->preview(it);
                    return TRUE;
                case GDK_KEY_e:
                    if (!it.image) self->edit(it);
                    return TRUE;
            }
        }
        // Other typing goes to the search field.
        if (!(mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) && gdk_keyval_to_unicode(ev->keyval) &&
            gtk_widget_get_visible(self->search_)) {
            gtk_entry_grab_focus_without_selecting(GTK_ENTRY(self->search_));
            return FALSE;  // the window now delivers the key to the entry
        }
    }
    return FALSE;
}

}  // namespace ci::ui
