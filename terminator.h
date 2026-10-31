#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <X11/Xft/Xft.h>

#define MAX_TABS 8
#define INPUT_CAP 4096
#define OUTPUT_CAP 65536
#define HISTORY_CAP 10000
#define MAX_COMMANDS 32
#define MAX_ARGS 128
#define MAX_JOBS 32
#define MAX_WATCH_COMMANDS 16
#define WATCH_OUTPUT_CAP 8192

typedef struct {
    char cwd[PATH_MAX];
    char input[INPUT_CAP];
    size_t cursor;
    char output[OUTPUT_CAP];
    size_t output_len;
    char multiline[INPUT_CAP];
    int continuing;
    int history_index;
    int scroll_offset;
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
    char command[INPUT_CAP];
    char cwd[PATH_MAX];
    pid_t pgid;
    pid_t pids[MAX_COMMANDS];
    int pid_count;
    int output_fd;
    int active;
    int running;
    int header_printed;
    long long next_run_ms;
} WatchCommand;

typedef struct {
    int active;
    int tab_index;
    int count;
    WatchCommand commands[MAX_WATCH_COMMANDS];
} MultiWatch;

typedef struct {
    Display *display;
    Window window;
    GC gc;

    XftFont *font;
    XftDraw *xft_draw;

    XftColor xft_foreground;
    XftColor xft_prompt_color;
    XftColor xft_accent;
    
    unsigned long foreground;
    unsigned long background;
    unsigned long tab_background;
    unsigned long active_tab_background;
    unsigned long accent;
    unsigned long prompt_color;
    unsigned long border_color;
    int width;
    int height;
    int running;
    int tab_count;
    int active_tab;
    Tab tabs[MAX_TABS];
    char **history;
    int history_count;
    int history_capacity;
    int exit_requested;
    pid_t foreground_pgid;
    Job jobs[MAX_JOBS];
    int next_job_id;
    MultiWatch multiwatch;
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
void history_load(App *app);
void history_save(App *app);
void history_free(App *app);
int multiwatch_start(App *app, Tab *tab, const char *arguments);
void multiwatch_poll(App *app);
void multiwatch_stop(App *app);

#endif
