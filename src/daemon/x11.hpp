// X11 clipboard access without a toolkit: watches CLIPBOARD ownership through
// XFixes (event driven, no polling), reads new contents (including INCR
// transfers), serves our own contents, grabs global hotkeys and fakes the
// paste keystroke through XTest.
#pragma once

#include <X11/Xlib.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glib.h>

namespace ci {

class X11 {
public:
    struct Callbacks {
        // New clipboard contents owned by another client.
        std::function<void(std::string mime, std::string data)> on_content;
        // The clipboard owner went away without handing the contents over.
        std::function<void()> on_owner_gone;
        // A grabbed hotkey (index into the list given to set_hotkeys).
        std::function<void(int index, Time time)> on_hotkey;
    };

    ~X11();
    bool init(Callbacks cb);

    // Takes CLIPBOARD ownership. Images are read from `path` on request.
    void own_text(std::string text);
    void own_image(std::string mime, std::string path);
    bool owns() const { return owned_; }
    bool has_foreign_owner();
    // Re-reads the current foreign owner's contents (used at startup).
    void fetch_current();

    // Returns the names of hotkeys that could not be grabbed.
    std::vector<std::string> set_hotkeys(std::span<const std::string> accels);

    // res_class and res_name of the active window, lowercase, "\n"-joined.
    std::string active_window_class();
    void send_paste(bool terminal);

    Time server_time();
    Time last_event_time() const { return last_time_; }

private:
    struct Transfer {  // outgoing INCR transfer
        Window requestor;
        Atom property, type;
        std::shared_ptr<const std::string> data;
        size_t offset;
        gint64 started;
    };
    struct Hotkey { KeyCode code; unsigned mods; };

    static gboolean dispatch_cb(int fd, GIOCondition, gpointer self);
    void pump();
    void handle(XEvent& ev);
    void on_owner_change(Window owner, Time t);
    void on_selection_notify(const XSelectionEvent& ev);
    void on_selection_request(const XSelectionRequestEvent& req);
    void on_property(const XPropertyEvent& ev);
    void start_fetch(Time t);
    void finish_fetch(bool ok);
    void send_data(Window req, Atom prop, Atom type, std::shared_ptr<const std::string> data);
    bool read_property(Atom prop, Atom& type, std::string& out, bool& incr);
    void take_ownership();
    std::vector<Atom> offered_targets() const;

    Display* dpy_ = nullptr;
    Window root_ = 0, win_ = 0;
    int xfixes_event_ = 0;
    guint source_ = 0;
    Callbacks cb_;
    Time last_time_ = CurrentTime;

    Atom CLIPBOARD, TARGETS, TIMESTAMP, INCR, UTF8_STRING, TEXT, MULTIPLE,
        PROP, TIME_PROP, NET_ACTIVE_WINDOW, TEXT_PLAIN, TEXT_PLAIN_UTF8,
        PASSWORD_HINT;

    // Incoming transfer state
    enum class Stage { Idle, Targets, Data, Incr } stage_ = Stage::Idle;
    Atom fetch_target_ = 0;
    std::string fetch_buf_;
    guint fetch_timeout_ = 0;
    Time fetch_time_ = CurrentTime;
    bool fetch_again_ = false;

    // Our contents
    bool owned_ = false;
    Time own_time_ = CurrentTime;
    bool own_image_ = false;
    std::string own_mime_;
    std::string own_data_;  // text, or image file path
    std::vector<Transfer> transfers_;
    size_t max_chunk_ = 0;

    std::vector<Hotkey> hotkeys_;
};

}  // namespace ci
