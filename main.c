#include "terminator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    App app;
    memset(&app, 0, sizeof(app));

    app.display = XOpenDisplay(NULL);
    if (app.display == NULL) {
        fprintf(stderr, "terminator: cannot open X display. Check that XWayland is running.\n");
        return 1;
    }

    int screen = DefaultScreen(app.display);
    app.width = 1000;
    app.height = 650;
    app.background = BlackPixel(app.display, screen);
    app.foreground = WhitePixel(app.display, screen);
    app.window = XCreateSimpleWindow(app.display, RootWindow(app.display, screen),
                                     80, 60, (unsigned)app.width, (unsigned)app.height,
                                     1, app.foreground, app.background);
    XStoreName(app.display, app.window, "Terminator");
    XSelectInput(app.display, app.window,
                 ExposureMask | KeyPressMask | StructureNotifyMask);

    app.font = XLoadQueryFont(app.display, "9x18");
    if (app.font == NULL)
        app.font = XLoadQueryFont(app.display, "fixed");
    app.gc = XCreateGC(app.display, app.window, 0, NULL);
    if (app.font != NULL)
        XSetFont(app.display, app.gc, app.font->fid);
    XSetForeground(app.display, app.gc, app.foreground);
    XMapWindow(app.display, app.window);

    app.running = 1;
    app.tab_count = 1;
    app.active_tab = 0;
    app.tabs[0].alive = 1;
    app.tabs[0].history_index = -1;
    if (getcwd(app.tabs[0].cwd, sizeof(app.tabs[0].cwd)) == NULL)
        snprintf(app.tabs[0].cwd, sizeof(app.tabs[0].cwd), "/");
    tab_append(&app.tabs[0], "Terminator - a small terminal written in C and Xlib\n");
    tab_append(&app.tabs[0], "Type commands. Ctrl+T opens a tab; Alt+1..Alt+8 switches tabs.\n\n");

    gui_run(&app);

    if (app.font != NULL)
        XFreeFont(app.display, app.font);
    XFreeGC(app.display, app.gc);
    XDestroyWindow(app.display, app.window);
    XCloseDisplay(app.display);
    return 0;
}
