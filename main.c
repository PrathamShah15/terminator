#include "terminator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <X11/Xutil.h>

static unsigned long named_color(Display *display, int screen, const char *name, unsigned long fallback)
{
    XColor color, exact;
    Colormap map = DefaultColormap(display, screen);
    if (XAllocNamedColor(display, map, name, &color, &exact))
        return color.pixel;
    return fallback;
}

static int allocate_xft_color(Display *display, int screen, const char *name, XftColor *color)
{
    return XftColorAllocName(display, DefaultVisual(display, screen), DefaultColormap(display, screen),
                             name, color);
}

static void setup_execution_mode(App *app, int argc, char *argv[])
{
    app->use_process_mode = 0; // Default: Thread Mode
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0) {
            app->use_process_mode = 1;
            break;
        }
    }

    if (app->use_process_mode) {
        printf("[Terminator] Running in Process Mode (useProcess=True)\n");
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_NAME);

        app->history_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
        if (app->history_sem == SEM_FAILED) {
            perror("sem_open failed");
        }
        app->shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (app->shm_fd >= 0) {
            ftruncate(app->shm_fd, sizeof(SharedHistory));
            app->shared_history = mmap(NULL, sizeof(SharedHistory),
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       app->shm_fd, 0);
            if (app->shared_history != MAP_FAILED) {
                memset(app->shared_history, 0, sizeof(SharedHistory));
            } else {
                app->shared_history = NULL;
            }
        }
        app->tab_shared_pool = mmap(NULL, sizeof(TabSharedState) * MAX_TABS,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (app->tab_shared_pool != MAP_FAILED) {
            memset(app->tab_shared_pool, 0, sizeof(TabSharedState) * MAX_TABS);
        } else {
            app->tab_shared_pool = NULL;
        }
    } else {
        printf("[Terminator] Running in Thread Mode (Default)\n");
        pthread_mutex_init(&app->history_mutex, NULL);
        app->shared_history = calloc(1, sizeof(SharedHistory));
        app->tab_shared_pool = calloc(MAX_TABS, sizeof(TabSharedState));
    }
}

static void cleanup_execution_mode(App *app)
{
    if (app->use_process_mode) {
        if (app->history_sem != SEM_FAILED && app->history_sem != NULL) {
            sem_close(app->history_sem);
            sem_unlink(SEM_NAME);
            app->history_sem = NULL;
        }
        if (app->shared_history && app->shared_history != MAP_FAILED) {
            munmap(app->shared_history, sizeof(SharedHistory));
            app->shared_history = NULL;
        }
        if (app->shm_fd >= 0) {
            close(app->shm_fd);
            shm_unlink(SHM_NAME);
            app->shm_fd = -1;
        }
        if (app->tab_shared_pool && app->tab_shared_pool != MAP_FAILED) {
            munmap(app->tab_shared_pool, sizeof(TabSharedState) * MAX_TABS);
            app->tab_shared_pool = NULL;
        }
    } else {
        pthread_mutex_destroy(&app->history_mutex);
        if (app->shared_history) {
            free(app->shared_history);
            app->shared_history = NULL;
        }
        if (app->tab_shared_pool) {
            free(app->tab_shared_pool);
            app->tab_shared_pool = NULL;
        }
    }
}

int main(int argc, char *argv[])
{
    App app;
    memset(&app, 0, sizeof(app));
    app.gui_thread_id = pthread_self();

    setup_execution_mode(&app, argc, argv);
    history_load(&app);

    app.display = XOpenDisplay(NULL);
    if (app.display == NULL)
    {
        fprintf(stderr, "terminator: cannot open X display. Check that XWayland is running.\n");
        cleanup_execution_mode(&app);
        return 1;
    }

    int screen = DefaultScreen(app.display);
    app.width = 1000;
    app.height = 650;
    app.background = BlackPixel(app.display, screen);
    app.foreground = WhitePixel(app.display, screen);
    app.tab_background = named_color(app.display, screen, "#202632", app.background);
    app.active_tab_background = named_color(app.display, screen, "#30445c", app.tab_background);
    app.accent = named_color(app.display, screen, "#56b6c2", app.foreground);
    app.prompt_color = named_color(app.display, screen, "#55d98a", app.foreground);
    app.border_color = named_color(app.display, screen, "#465366", app.foreground);
    app.window = XCreateSimpleWindow(app.display, RootWindow(app.display, screen),
                                     80, 60, (unsigned)app.width, (unsigned)app.height,
                                     1, app.foreground, app.background);
    XStoreName(app.display, app.window, "Terminator");

    app.clipboard_atom = XInternAtom(app.display, "CLIPBOARD", False);
    app.utf8_string_atom = XInternAtom(app.display, "UTF8_STRING", False);
    app.targets_atom = XInternAtom(app.display, "TARGETS", False);
    app.paste_property = XInternAtom(app.display, "TERMINATOR_CLIPBOARD", False);
    XSelectInput(app.display, app.window,
                 ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);

    app.font = XftFontOpenName(app.display, screen, "DejaVu Sans Mono:size=16");

    if (app.font == NULL)
    {
        fprintf(stderr, "Could not load Xft font\n");
        XDestroyWindow(app.display, app.window);
        XCloseDisplay(app.display);
        history_save(&app);
        history_free(&app);
        cleanup_execution_mode(&app);
        return 1;
    }

    app.xft_draw = XftDrawCreate(app.display, app.window, DefaultVisual(app.display, screen),
                                 DefaultColormap(app.display, screen));

    if (app.xft_draw == NULL)
    {
        fprintf(stderr, "Could not create Xft drawing context\n");
        XftFontClose(app.display, app.font);
        XDestroyWindow(app.display, app.window);
        XCloseDisplay(app.display);
        history_save(&app);
        history_free(&app);
        cleanup_execution_mode(&app);
        return 1;
    }

    if (!allocate_xft_color(app.display, screen, "#ffffff",
                            &app.xft_foreground) ||
        !allocate_xft_color(app.display, screen, "#55d98a",
                            &app.xft_prompt_color) ||
        !allocate_xft_color(app.display, screen, "#56b6c2",
                            &app.xft_accent))
    {
        fprintf(stderr, "Could not allocate Xft colors\n");
        XftDrawDestroy(app.xft_draw);
        XftFontClose(app.display, app.font);
        XDestroyWindow(app.display, app.window);
        XCloseDisplay(app.display);
        history_save(&app);
        history_free(&app);
        cleanup_execution_mode(&app);
        return 1;
    }

    app.gc = XCreateGC(app.display, app.window, 0, NULL);
    XSetForeground(app.display, app.gc, app.foreground);
    XMapWindow(app.display, app.window);

    app.running = 1;
    create_tab(&app);

    tab_append(&app.tabs[0], "Type commands. Ctrl+T opens a tab; Alt+1...Alt+8 switches tabs.\n\n");

    gui_run(&app);

    XftColorFree(app.display, DefaultVisual(app.display, screen), DefaultColormap(app.display, screen),
                 &app.xft_foreground);
    XftColorFree(app.display, DefaultVisual(app.display, screen), DefaultColormap(app.display, screen),
                 &app.xft_prompt_color);
    XftColorFree(app.display, DefaultVisual(app.display, screen), DefaultColormap(app.display, screen),
                 &app.xft_accent);

    XftDrawDestroy(app.xft_draw);
    if (app.backbuffer != None)
        XFreePixmap(app.display, app.backbuffer);
    XftFontClose(app.display, app.font);
    XFreeGC(app.display, app.gc);

    XDestroyWindow(app.display, app.window);
    history_save(&app);
    history_free(&app);
    hotkeys_cleanup(&app);
    cleanup_execution_mode(&app);
    XCloseDisplay(app.display);
    return 0;
}
