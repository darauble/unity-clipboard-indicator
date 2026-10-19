#include "x11.hpp"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstring>

#include <glib-unix.h>

namespace ci {

namespace {

bool g_x_error = false;

int on_x_error(Display*, XErrorEvent* e) {
    // Requestors may vanish mid-transfer; grabs may be taken. Never fatal.
    g_x_error = true;
    if (e->error_code != BadWindow && e->error_code != BadAccess)
        g_debug("X error %d (request %d)", e->error_code, e->request_code);
    return 0;
}

constexpr const char* kPreferredTargets[] = {
    "UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "STRING",
    "image/png", "image/gif", "image/jpeg", "image/jpg", "image/webp",
    "image/svg+xml", "text/html",
};

constexpr size_t kMaxIncoming = 256u << 20;
constexpr unsigned kModMask = ShiftMask | ControlMask | Mod1Mask | Mod4Mask;

std::string lower(const char* s) {
    std::string r = s ? s : "";
    for (auto& c : r) c = g_ascii_tolower(c);
    return r;
}

bool parse_accel(Display* dpy, const std::string& accel, KeyCode& code, unsigned& mods) {
    mods = 0;
    size_t i = 0;
    while (i < accel.size() && accel[i] == '<') {
        size_t end = accel.find('>', i);
        if (end == std::string::npos) return false;
        std::string m = lower(accel.substr(i + 1, end - i - 1).c_str());
        if (m == "control" || m == "ctrl" || m == "primary") mods |= ControlMask;
        else if (m == "shift") mods |= ShiftMask;
        else if (m == "alt" || m == "mod1") mods |= Mod1Mask;
        else if (m == "super" || m == "meta" || m == "hyper" || m == "mod4") mods |= Mod4Mask;
        else return false;
        i = end + 1;
    }
    std::string key = accel.substr(i);
    if (key.empty()) return false;
    KeySym sym = XStringToKeysym(key.c_str());
    if (sym == NoSymbol) sym = XStringToKeysym(lower(key.c_str()).c_str());
    if (sym == NoSymbol) return false;
    code = XKeysymToKeycode(dpy, sym);
    return code != 0;
}

}  // namespace

X11::~X11() {
    if (source_) g_source_remove(source_);
    if (fetch_timeout_) g_source_remove(fetch_timeout_);
    if (dpy_) XCloseDisplay(dpy_);
}

bool X11::init(Callbacks cb) {
    cb_ = std::move(cb);
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) return false;
    XSetErrorHandler(on_x_error);

    int err_base;
    if (!XFixesQueryExtension(dpy_, &xfixes_event_, &err_base)) {
        g_warning("XFixes extension missing");
        return false;
    }

    root_ = DefaultRootWindow(dpy_);
    win_ = XCreateSimpleWindow(dpy_, root_, -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(dpy_, win_, PropertyChangeMask);

    auto atom = [&](const char* n) { return XInternAtom(dpy_, n, False); };
    CLIPBOARD = atom("CLIPBOARD");
    TARGETS = atom("TARGETS");
    TIMESTAMP = atom("TIMESTAMP");
    INCR = atom("INCR");
    UTF8_STRING = atom("UTF8_STRING");
    TEXT = atom("TEXT");
    MULTIPLE = atom("MULTIPLE");
    PROP = atom("CI_SELECTION");
    TIME_PROP = atom("CI_TIME");
    NET_ACTIVE_WINDOW = atom("_NET_ACTIVE_WINDOW");
    TEXT_PLAIN = atom("text/plain");
    TEXT_PLAIN_UTF8 = atom("text/plain;charset=utf-8");
    PASSWORD_HINT = atom("x-kde-passwordManagerHint");

    long max_req = XExtendedMaxRequestSize(dpy_);
    if (!max_req) max_req = XMaxRequestSize(dpy_);
    max_chunk_ = std::min<size_t>(size_t(max_req) * 4 - 1024, 256 * 1024);

    XFixesSelectSelectionInput(dpy_, win_, CLIPBOARD,
                               XFixesSetSelectionOwnerNotifyMask |
                                   XFixesSelectionWindowDestroyNotifyMask |
                                   XFixesSelectionClientCloseNotifyMask);

    source_ = g_unix_fd_add(ConnectionNumber(dpy_), G_IO_IN, dispatch_cb, this);
    XFlush(dpy_);
    return true;
}

gboolean X11::dispatch_cb(int, GIOCondition, gpointer self) {
    static_cast<X11*>(self)->pump();
    return G_SOURCE_CONTINUE;
}

// Drains Xlib's queue. Round trips made elsewhere may have queued events
// without the fd becoming readable again, so public entry points call this
// too.
void X11::pump() {
    while (XPending(dpy_)) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        handle(ev);
    }
    XFlush(dpy_);
}

void X11::handle(XEvent& ev) {
    if (ev.type == xfixes_event_ + XFixesSelectionNotify) {
        auto& n = reinterpret_cast<XFixesSelectionNotifyEvent&>(ev);
        if (n.selection == CLIPBOARD)
            on_owner_change(n.subtype == XFixesSetSelectionOwnerNotify ? n.owner : None, n.timestamp);
        return;
    }
    switch (ev.type) {
        case SelectionNotify: on_selection_notify(ev.xselection); break;
        case SelectionRequest: on_selection_request(ev.xselectionrequest); break;
        case SelectionClear:
            if (ev.xselectionclear.selection == CLIPBOARD) {
                owned_ = false;
                std::string().swap(own_data_);
            }
            break;
        case PropertyNotify: on_property(ev.xproperty); break;
        case KeyPress: {
            last_time_ = ev.xkey.time;
            unsigned state = ev.xkey.state & kModMask;
            for (size_t i = 0; i < hotkeys_.size(); ++i)
                if (hotkeys_[i].code && hotkeys_[i].code == ev.xkey.keycode && hotkeys_[i].mods == state)
                    cb_.on_hotkey(int(i), ev.xkey.time);
            break;
        }
        case MappingNotify: XRefreshKeyboardMapping(&ev.xmapping); break;
    }
}

// ---------------------------------------------------------------------------
// Reading foreign clipboard contents

void X11::on_owner_change(Window owner, Time t) {
    if (t != CurrentTime) last_time_ = t;
    if (owner == win_) return;
    if (owner == None) {
        if (!owned_ && cb_.on_owner_gone) cb_.on_owner_gone();
        return;
    }
    start_fetch(t);
}

bool X11::has_foreign_owner() {
    Window w = XGetSelectionOwner(dpy_, CLIPBOARD);
    return w != None && w != win_;
}

void X11::fetch_current() {
    if (has_foreign_owner()) start_fetch(CurrentTime);
    pump();
}

void X11::start_fetch(Time t) {
    if (stage_ != Stage::Idle) {
        fetch_again_ = true;  // restart once the transfer in flight is done
        return;
    }
    stage_ = Stage::Targets;
    fetch_time_ = t;
    XConvertSelection(dpy_, CLIPBOARD, TARGETS, PROP, win_, t);
    fetch_timeout_ = g_timeout_add_seconds(3, [](gpointer self) -> gboolean {
        auto* x = static_cast<X11*>(self);
        x->fetch_timeout_ = 0;
        x->finish_fetch(false);
        return G_SOURCE_REMOVE;
    }, this);
}

void X11::finish_fetch(bool ok) {
    if (fetch_timeout_) {
        g_source_remove(fetch_timeout_);
        fetch_timeout_ = 0;
    }
    stage_ = Stage::Idle;
    if (ok && cb_.on_content) {
        char* name = XGetAtomName(dpy_, fetch_target_);
        std::string mime = name ? name : "";
        XFree(name);
        std::string data;
        data.swap(fetch_buf_);
        if (mime == "STRING") {
            if (char* u = g_convert(data.data(), gssize(data.size()), "UTF-8", "ISO-8859-1", nullptr, nullptr, nullptr)) {
                data = u;
                g_free(u);
            }
            mime = "text/plain;charset=utf-8";
        } else if (mime == "UTF8_STRING") {
            mime = "text/plain;charset=utf-8";
        }
        cb_.on_content(std::move(mime), std::move(data));
    }
    std::string().swap(fetch_buf_);
    if (fetch_again_) {
        fetch_again_ = false;
        start_fetch(CurrentTime);
    }
}

bool X11::read_property(Atom prop, Atom& type, std::string& out, bool& incr) {
    out.clear();
    incr = false;
    int format;
    unsigned long n, after;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy_, win_, prop, 0, LONG_MAX / 4, True, AnyPropertyType,
                           &type, &format, &n, &after, &data) != Success)
        return false;
    if (type == INCR) {
        incr = true;
    } else if (data && n) {
        size_t unit = format == 8 ? 1 : format == 16 ? sizeof(short) : sizeof(long);
        if (format == 8) out.assign(reinterpret_cast<char*>(data), n);
        else out.assign(reinterpret_cast<char*>(data), n * unit);
    }
    if (data) XFree(data);
    return true;
}

void X11::on_selection_notify(const XSelectionEvent& ev) {
    if (ev.requestor != win_ || ev.selection != CLIPBOARD) return;

    if (stage_ == Stage::Targets) {
        Atom target = None;
        if (ev.property == None) {
            target = UTF8_STRING;  // owner without TARGETS support
        } else {
            int format;
            unsigned long n, after;
            unsigned char* data = nullptr;
            Atom type;
            XGetWindowProperty(dpy_, win_, PROP, 0, 4096, True, XA_ATOM, &type, &format, &n, &after, &data);
            auto* atoms = reinterpret_cast<Atom*>(data);
            std::span<Atom> offered(atoms, data && format == 32 ? n : 0);
            bool secret = std::ranges::find(offered, PASSWORD_HINT) != offered.end();
            for (const char* name : kPreferredTargets) {
                Atom a = XInternAtom(dpy_, name, True);
                if (a != None && std::ranges::find(offered, a) != offered.end()) {
                    target = a;
                    break;
                }
            }
            if (data) XFree(data);
            if (secret) target = None;  // password managers mark secrets
        }
        if (target == None) return finish_fetch(false);
        fetch_target_ = target;
        stage_ = Stage::Data;
        XConvertSelection(dpy_, CLIPBOARD, target, PROP, win_, fetch_time_);
        return;
    }

    if (stage_ == Stage::Data) {
        if (ev.property == None) return finish_fetch(false);
        Atom type;
        bool incr;
        if (!read_property(PROP, type, fetch_buf_, incr)) return finish_fetch(false);
        if (incr) {
            stage_ = Stage::Incr;  // deleting the property started the transfer
            fetch_buf_.clear();
            return;
        }
        finish_fetch(!fetch_buf_.empty());
    }
}

void X11::on_property(const XPropertyEvent& ev) {
    if (ev.window == win_) {
        if (ev.atom == PROP && ev.state == PropertyNewValue && stage_ == Stage::Incr) {
            Atom type;
            bool incr;
            std::string chunk;
            if (!read_property(PROP, type, chunk, incr)) return finish_fetch(false);
            if (chunk.empty()) return finish_fetch(!fetch_buf_.empty());
            if (fetch_buf_.size() + chunk.size() > kMaxIncoming) return finish_fetch(false);
            fetch_buf_ += chunk;
        }
        return;
    }
    if (ev.state != PropertyDelete) return;
    for (auto it = transfers_.begin(); it != transfers_.end(); ++it) {
        if (it->requestor != ev.window || it->property != ev.atom) continue;
        size_t len = std::min(max_chunk_, it->data->size() - it->offset);
        XChangeProperty(dpy_, it->requestor, it->property, it->type, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(it->data->data()) + it->offset, int(len));
        it->offset += len;
        if (len == 0) {  // the zero-length write terminates the transfer
            Window w = it->requestor;
            transfers_.erase(it);
            if (std::ranges::find(transfers_, w, &Transfer::requestor) == transfers_.end())
                XSelectInput(dpy_, w, NoEventMask);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Serving our contents

Time X11::server_time() {
    XChangeProperty(dpy_, win_, TIME_PROP, XA_STRING, 8, PropModeAppend, nullptr, 0);
    XEvent ev;
    struct Match { Window w; Atom a; } m{win_, TIME_PROP};
    XIfEvent(dpy_, &ev, [](Display*, XEvent* e, XPointer p) -> Bool {
        auto* m = reinterpret_cast<Match*>(p);
        return e->type == PropertyNotify && e->xproperty.window == m->w && e->xproperty.atom == m->a;
    }, reinterpret_cast<XPointer>(&m));
    return ev.xproperty.time;
}

void X11::take_ownership() {
    own_time_ = server_time();
    XSetSelectionOwner(dpy_, CLIPBOARD, win_, own_time_);
    owned_ = XGetSelectionOwner(dpy_, CLIPBOARD) == win_;
    if (!owned_) g_warning("could not take clipboard ownership");
    pump();
}

void X11::own_text(std::string text) {
    own_image_ = false;
    own_mime_ = "text/plain;charset=utf-8";
    own_data_ = std::move(text);
    take_ownership();
}

void X11::own_image(std::string mime, std::string path) {
    own_image_ = true;
    own_mime_ = std::move(mime);
    own_data_ = std::move(path);
    take_ownership();
}

std::vector<Atom> X11::offered_targets() const {
    std::vector<Atom> t{TARGETS, TIMESTAMP};
    if (own_image_) {
        t.push_back(XInternAtom(dpy_, own_mime_.c_str(), False));
    } else {
        t.insert(t.end(), {UTF8_STRING, TEXT_PLAIN_UTF8, TEXT_PLAIN, XA_STRING, TEXT});
    }
    return t;
}

void X11::on_selection_request(const XSelectionRequestEvent& req) {
    XSelectionEvent ev{};
    ev.type = SelectionNotify;
    ev.display = dpy_;
    ev.requestor = req.requestor;
    ev.selection = req.selection;
    ev.target = req.target;
    ev.time = req.time;
    ev.property = None;
    Atom prop = req.property != None ? req.property : req.target;

    bool valid = owned_ && req.selection == CLIPBOARD &&
                 (req.time == CurrentTime || req.time >= own_time_);
    if (valid) {
        auto targets = offered_targets();
        bool offered = std::ranges::find(targets, req.target) != targets.end();
        if (req.target == TARGETS) {
            XChangeProperty(dpy_, req.requestor, prop, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(targets.data()), int(targets.size()));
            ev.property = prop;
        } else if (req.target == TIMESTAMP) {
            long t = long(own_time_);
            XChangeProperty(dpy_, req.requestor, prop, XA_INTEGER, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(&t), 1);
            ev.property = prop;
        } else if (offered && own_image_) {
            char* raw = nullptr;
            gsize len = 0;
            if (g_file_get_contents(own_data_.c_str(), &raw, &len, nullptr)) {
                send_data(req.requestor, prop, req.target, std::make_shared<const std::string>(raw, len));
                g_free(raw);
                ev.property = prop;
            }
        } else if (offered) {
            auto data = std::make_shared<std::string>(own_data_);
            if (req.target == XA_STRING) {
                if (char* l = g_convert_with_fallback(own_data_.data(), gssize(own_data_.size()), "ISO-8859-1",
                                                      "UTF-8", "?", nullptr, nullptr, nullptr)) {
                    *data = l;
                    g_free(l);
                }
            }
            send_data(req.requestor, prop, req.target == TEXT ? UTF8_STRING : req.target, std::move(data));
            ev.property = prop;
        }
    }
    XSendEvent(dpy_, req.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&ev));
}

void X11::send_data(Window req, Atom prop, Atom type, std::shared_ptr<const std::string> data) {
    if (data->size() <= max_chunk_) {
        XChangeProperty(dpy_, req, prop, type, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(data->data()), int(data->size()));
        return;
    }
    gint64 now = g_get_monotonic_time();
    std::erase_if(transfers_, [&](const Transfer& t) { return now - t.started > 30 * G_USEC_PER_SEC; });
    XSelectInput(dpy_, req, PropertyChangeMask);
    long size = long(data->size());
    XChangeProperty(dpy_, req, prop, INCR, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&size), 1);
    transfers_.push_back({req, prop, type, std::move(data), 0, now});
}

// ---------------------------------------------------------------------------
// Hotkeys, active window, paste

std::vector<std::string> X11::set_hotkeys(std::span<const std::string> accels) {
    static constexpr unsigned kLockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (auto& h : hotkeys_)
        if (h.code)
            for (unsigned v : kLockVariants) XUngrabKey(dpy_, h.code, h.mods | v, root_);
    hotkeys_.assign(accels.size(), Hotkey{0, 0});

    std::vector<std::string> failed;
    for (size_t i = 0; i < accels.size(); ++i) {
        if (accels[i].empty()) continue;
        Hotkey h{};
        if (!parse_accel(dpy_, accels[i], h.code, h.mods)) {
            failed.push_back(accels[i]);
            continue;
        }
        XSync(dpy_, False);
        g_x_error = false;
        for (unsigned v : kLockVariants)
            XGrabKey(dpy_, h.code, h.mods | v, root_, False, GrabModeAsync, GrabModeAsync);
        XSync(dpy_, False);
        if (g_x_error) {
            failed.push_back(accels[i]);
            for (unsigned v : kLockVariants) XUngrabKey(dpy_, h.code, h.mods | v, root_);
            continue;
        }
        hotkeys_[i] = h;
    }
    pump();
    return failed;
}

std::string X11::active_window_class() {
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char* data = nullptr;
    std::string result;
    if (XGetWindowProperty(dpy_, root_, NET_ACTIVE_WINDOW, 0, 1, False, XA_WINDOW, &type, &format,
                           &n, &after, &data) == Success && data && n == 1) {
        Window w = *reinterpret_cast<Window*>(data);
        XClassHint hint{};
        if (w && XGetClassHint(dpy_, w, &hint)) {
            result = lower(hint.res_class) + "\n" + lower(hint.res_name);
            XFree(hint.res_name);
            XFree(hint.res_class);
        }
    }
    if (data) XFree(data);
    pump();
    return result;
}

void X11::send_paste(bool terminal) {
    auto key = [&](KeySym sym, bool down) {
        if (KeyCode c = XKeysymToKeycode(dpy_, sym)) XTestFakeKeyEvent(dpy_, c, down, CurrentTime);
    };
    if (terminal) key(XK_Control_L, true);
    key(XK_Shift_L, true);
    key(XK_Insert, true);
    key(XK_Insert, false);
    key(XK_Shift_L, false);
    if (terminal) key(XK_Control_L, false);
    XFlush(dpy_);
}

}  // namespace ci
