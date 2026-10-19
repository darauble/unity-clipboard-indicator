// clipboard-indicator-ui: started on demand by the daemon (hotkey, middle
// click or menu item) and exits when its windows close, so it costs nothing
// while hidden. A second invocation is forwarded to the running instance,
// which makes the hotkey toggle the popup.
//
//   clipboard-indicator-ui                 toggle the menu popup
//   clipboard-indicator-ui --at-cursor     ... opened at the mouse pointer
//   clipboard-indicator-ui --confirm-clear ask, then clear the history
//   clipboard-indicator-ui --settings      open the settings window
#include <cstring>

#include "common/settings.hpp"
#include "popup.hpp"
#include "style.hpp"

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new(ci::kUiAppId, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "startup", G_CALLBACK(+[](GApplication*, gpointer) { ci::ui::install_css(); }), nullptr);
    auto on_command_line = +[](GApplication* a, GApplicationCommandLine* cl, gpointer) -> int {
        int n = 0;
        char** args = g_application_command_line_get_arguments(cl, &n);
        bool settings = false, confirm = false, at_cursor = false;
        for (int i = 1; i < n; ++i) {
            if (!strcmp(args[i], "--settings")) settings = true;
            else if (!strcmp(args[i], "--confirm-clear")) confirm = true;
            else if (!strcmp(args[i], "--at-cursor")) at_cursor = true;
        }
        g_strfreev(args);
        auto* app = GTK_APPLICATION(a);
        if (settings) ci::ui::open_settings(app);
        else ci::ui::Popup::toggle(app, confirm ? ci::ui::Popup::Mode::ConfirmClear : ci::ui::Popup::Mode::Menu, at_cursor);
        return 0;
    };
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
