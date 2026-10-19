#include "style.hpp"

namespace ci::ui {

namespace {

// Mirrors the GNOME Shell popup menu and the extension's stylesheet.css.
// Everything is scoped to .ci-popup so the settings window keeps the
// system theme.
constexpr const char* kCss = R"CSS(
.ci-popup, .ci-overlay { background-color: transparent; }

.ci-frame {
  background-color: #353539;
  background-image: none;
  color: #ffffff;
  border: 1px solid rgba(255, 255, 255, 0.10);
  border-radius: 16px;
  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.55);
  margin: 12px;
  padding: 10px;
  font-size: 11pt;
}
.ci-frame label { color: #ffffff; text-shadow: none; }

.ci-search {
  min-height: 34px;
  padding: 0 10px;
  margin: 4px 2px 8px 2px;
  border-radius: 12px;
  border: 2px solid transparent;
  background-color: rgba(255, 255, 255, 0.08);
  background-image: none;
  box-shadow: none;
  color: #ffffff;
  caret-color: #ffffff;
}
.ci-search:focus { border-color: #78aeed; background-color: rgba(255, 255, 255, 0.10); }
.ci-search image { color: #ffffff; }

.ci-frame list, .ci-frame scrolledwindow, .ci-frame viewport {
  background-color: transparent;
  background-image: none;
  border: none;
}
.ci-frame list row {
  background-color: transparent;
  background-image: none;
  border-radius: 10px;
  padding: 2px 6px 2px 4px;
  min-height: 38px;
  outline-color: transparent;
  box-shadow: none;
}
.ci-frame list row:hover,
.ci-frame list row:focus,
.ci-frame list row:selected { background-color: rgba(255, 255, 255, 0.10); }

.ci-btn {
  background-color: transparent;
  background-image: none;
  border: none;
  box-shadow: none;
  border-radius: 999px;
  padding: 5px;
  margin: 0 1px;
  min-width: 0;
  min-height: 0;
  outline-color: transparent;
}
.ci-btn:hover { background-color: rgba(255, 255, 255, 0.12); }
.ci-btn:active { background-color: rgba(255, 255, 255, 0.20); }
.ci-btn.ci-dim image { opacity: 0.3; }

.ci-thumb { border: 1px solid #ffffff; }

.ci-tag {
  font-size: 0.8em;
  color: rgba(255, 255, 255, 0.6);
  background-color: rgba(255, 255, 255, 0.1);
  border-radius: 4px;
  padding: 1px 6px;
  margin: 0 6px;
}

.ci-sep {
  background-color: rgba(255, 255, 255, 0.12);
  min-height: 1px;
  margin: 6px 4px;
}

.ci-row {
  background-color: transparent;
  background-image: none;
  border: none;
  box-shadow: none;
  border-radius: 10px;
  padding: 6px 8px;
  min-height: 30px;
  outline-color: transparent;
}
.ci-row:hover, .ci-row:focus { background-color: rgba(255, 255, 255, 0.10); }
.ci-row label { font-weight: normal; }

.ci-timer { font-family: monospace; color: rgba(255, 255, 255, 0.8); }

.ci-empty { color: #aaaaaa; min-width: 350px; }
.ci-empty label { color: #aaaaaa; margin: 12px 0 30px 0; }

.ci-frame switch {
  background-color: rgba(255, 255, 255, 0.15);
  background-image: none;
  border: none;
  border-radius: 14px;
  box-shadow: none;
  min-width: 48px;
  min-height: 26px;
  color: transparent;
  font-size: 0;
}
.ci-frame switch:checked { background-color: #3584e4; }
.ci-frame switch slider {
  background-color: #e6e6e6;
  background-image: none;
  border: none;
  border-radius: 999px;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.3);
  min-width: 22px;
  min-height: 22px;
  margin: 2px;
}

.ci-dialog-title { font-weight: bold; }
.ci-dialog-text { padding-top: 12px; }
.ci-dialog-btn {
  background-color: rgba(255, 255, 255, 0.10);
  background-image: none;
  border: none;
  box-shadow: none;
  border-radius: 10px;
  padding: 8px 16px;
  min-height: 24px;
  color: #ffffff;
  outline-color: transparent;
}
.ci-dialog-btn:hover { background-color: rgba(255, 255, 255, 0.16); }
.ci-dialog-btn:focus { box-shadow: inset 0 0 0 2px #78aeed; }
.ci-dialog-btn label { font-weight: bold; }

.ci-entry, .ci-edit {
  background-color: rgba(255, 255, 255, 0.10);
  background-image: none;
  border: 2px solid transparent;
  border-radius: 6px;
  box-shadow: none;
  color: #ffffff;
  caret-color: #ffffff;
  padding: 6px 8px;
}
.ci-entry:focus { border-color: #78aeed; }
.ci-edit text { background-color: transparent; color: #ffffff; }
.ci-edit text selection, .ci-entry selection { background-color: rgba(100, 150, 255, 0.71); color: #ffffff; }
.ci-edit-scroll { border-radius: 6px; background-color: rgba(255, 255, 255, 0.10); }

.ci-overlay-bg { background-color: rgba(0, 0, 0, 0.75); }
)CSS";

GtkIconTheme* adwaita() {
    static GtkIconTheme* theme = [] {
        GtkIconTheme* t = gtk_icon_theme_new();
        gtk_icon_theme_set_custom_theme(t, "Adwaita");
        return t;
    }();
    return theme;
}

}  // namespace

void install_css() {
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, kCss, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

GtkWidget* icon(const char* name, int size, const char* color) {
    GdkRGBA rgba;
    gdk_rgba_parse(&rgba, color);
    int scale = 1;
    if (GdkMonitor* m = gdk_display_get_primary_monitor(gdk_display_get_default()))
        scale = gdk_monitor_get_scale_factor(m);
    GtkIconInfo* info = gtk_icon_theme_lookup_icon_for_scale(adwaita(), name, size, scale,
                                                             GTK_ICON_LOOKUP_FORCE_SIZE);
    if (!info) return gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_MENU);
    GdkPixbuf* pb = gtk_icon_info_load_symbolic(info, &rgba, nullptr, nullptr, nullptr, nullptr, nullptr);
    g_object_unref(info);
    if (!pb) return gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_MENU);
    cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(pb, scale, nullptr);
    GtkWidget* img = gtk_image_new_from_surface(surface);
    cairo_surface_destroy(surface);
    g_object_unref(pb);
    return img;
}

}  // namespace ci::ui
