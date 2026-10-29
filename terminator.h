#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_TABS 8
#define INPUT_CAP 4096
#define OUTPUT_CAP 65536
#define HISTORY_CAP 100
#define MAX_COMMANDS 32
#define MAX_ARGS 128
#define MAX_JOBS 32

typedef struct {
    char cwd[PATH_MAX];
    char input[INPUT_CAP];
    size_t cursor;
    char output[OUTPUT_CAP];
    size_t output_len;
    char multiline[INPUT_CAP];
    int continuing;
    int history_index;
    int alive;
} Tab;

typedef struct {
    pid_t pgid;
    int output_fd;
    int active;
    int stopped;
    int tab_index;
    char command[INPUT_CAP];
} Job;

typedef struct {
    Display *display;
    Window window;
    GC gc;
    XFontStruct *font;
    unsigned long foreground;
    unsigned long background;
    int width;
    int height;
    int running;
    int tab_count;
    int active_tab;
    Tab tabs[MAX_TABS];
    char history[HISTORY_CAP][INPUT_CAP];
    int history_count;
    int exit_requested;
    pid_t foreground_pgid;
    Job jobs[MAX_JOBS];
    int next_job_id;
} App;

void gui_run(App *app);
void gui_redraw(App *app);
void shell_execute(App *app, Tab *tab, const char *line);
int builtin_execute(App *app, Tab *tab, const char *line);
int parser_execute_pipeline(App *app, Tab *tab, const char *line);
int job_control_builtin(App *app, Tab *tab, const char *line);
void jobs_poll(App *app);
int hotkey_handle(App *app, XKeyEvent *event, KeySym key, const char *text, int text_len);
void tab_append(Tab *tab, const char *text);
void tab_clear(Tab *tab);
void history_add(App *app, const char *line);

#endif
