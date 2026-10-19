// The clipboard menu popup, modelled on the GNOME Shell extension's menu:
// search, pinned + history rows with item actions, private mode, settings
// and clear history. It is an override-redirect window holding a pointer and
// keyboard grab like a real menu, so the previously focused application
// keeps focus and receives the paste keystroke.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "common/settings.hpp"

namespace ci::ui {

class Popup {
public:
    enum class Mode { Menu, ConfirmClear };
    // Opens the popup, or closes it if it is already open.
    static void toggle(GtkApplication* app, Mode mode, bool at_cursor);

private:
    struct Item {
        uint32_t id;
        bool image, pinned, selected;
        std::string mime, data, tag;
        std::string folded;  // casefolded text + tag, computed on first search
    };
    struct Action {
        Popup* self;
        uint32_t id;
        void (Popup::*fn)(const Item&);
    };

    Popup(GtkApplication* app, Mode mode, bool at_cursor);
    ~Popup();
    bool start();

    bool fetch();
    void build();
    GtkWidget* build_menu_page();
    GtkWidget* build_edit_page();
    GtkWidget* build_tag_page();
    GtkWidget* build_confirm_page();
    GtkWidget* dialog_buttons(const char* cancel, const char* ok, void (Popup::*on_ok)(), GtkWidget** ok_out);
    GtkWidget* menu_row(const char* icon_name, const char* label, GtkWidget* extra);
    GtkWidget* action_button(const char* icon_name, uint32_t id, void (Popup::*fn)(const Item&), bool dim);
    void rebuild_list();
    GtkWidget* make_row(const Item& it, int section);
    void update_visibility();
    void apply_filter();
    bool matches(Item& it);
    void show_page(const char* name, GtkWidget* focus);
    void reposition();
    void grab(GtkWidget* w);
    void ungrab();
    void close();
    bool call(const char* method, GVariant* args = nullptr);
    void on_changed();
    void update_timer();

    // Item actions
    void activate(const Item& it);
    void paste(const Item& it);
    void toggle_pin(const Item& it);
    void remove(const Item& it);
    void edit(const Item& it);
    void tag(const Item& it);
    void preview(const Item& it);
    void close_preview();

    // Page actions
    void save_edit();
    void save_tag();
    void confirm_ok();
    void cancel_page();
    void confirm(const char* title, const char* text, const char* ok, std::function<void()> action);
    void request_clear();

    Item* find(uint32_t id);
    Item* item_for_row(GtkWidget* row);
    GtkWidget* row_for_id(uint32_t id);
    GtkWidget* first_visible_row();
    void focus_neighbor_of(uint32_t id);

    static gboolean on_key(GtkWidget*, GdkEventKey*, gpointer);
    static gboolean on_button(GtkWidget*, GdkEventButton*, gpointer);

    static Popup* current_;

    GtkApplication* app_;
    Mode mode_;
    bool at_cursor_;
    Settings s_;
    GDBusConnection* conn_ = nullptr;
    guint changed_sub_ = 0, timer_ = 0, grab_retry_ = 0;
    int grab_tries_ = 0;

    std::vector<Item> items_;
    bool private_ = false;
    gint64 next_clear_ = -1;
    std::string filter_;
    GRegex* regex_ = nullptr;
    uint32_t page_item_ = 0;     // item being edited/tagged
    uint32_t focus_after_ = 0;   // row to focus after the next rebuild
    std::function<void()> confirm_action_;
    bool updating_ = false;
    GdkRectangle workarea_{};
    int pointer_x_ = 0, pointer_y_ = 0;

    GtkWidget *win_ = nullptr, *frame_ = nullptr, *stack_ = nullptr;
    GtkWidget *search_ = nullptr, *scroller_ = nullptr, *list_ = nullptr, *empty_ = nullptr;
    GtkWidget *bottom_sep_ = nullptr, *private_row_ = nullptr, *private_switch_ = nullptr;
    GtkWidget *settings_row_ = nullptr, *clear_row_ = nullptr, *timer_label_ = nullptr, *reset_btn_ = nullptr;
    GtkWidget *edit_view_ = nullptr, *edit_save_ = nullptr, *tag_entry_ = nullptr, *tag_save_ = nullptr;
    GtkWidget *confirm_title_ = nullptr, *confirm_text_ = nullptr, *confirm_ok_ = nullptr;
    GtkWidget* preview_ = nullptr;
};

void open_settings(GtkApplication* app);

}  // namespace ci::ui
