#pragma once

#include <gtk/gtk.h>

namespace ci::ui {

void install_css();

// Adwaita symbolic icon rendered in `color` (white by default), independent
// of the user's icon theme, so the popup looks like the GNOME Shell menu.
GtkWidget* icon(const char* name, int size, const char* color = "#ffffff");

}  // namespace ci::ui
