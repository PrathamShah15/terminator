#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <X11/Xft/Xft.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_TABS 8
#define INPUT_CAP 4096
#define OUTPUT_CAP 65536
#define HISTORY_CAP 10000
#define MAX_COMMANDS 32
#define MAX_ARGS 128
#define MAX_JOBS 32
#define MAX_WATCH_COMMANDS 16
#define WATCH_OUTPUT_CAP 8192
#define MAX_COMPLETIONS 128
#define SHM_NAME "/terminator_history_shm"
#define SEM_NAME "/terminator_history_sem"

typedef struct App App;

typedef struct {
    char cwd[PATH_MAX];
    pid_t foreground_pgid;
    int busy;
} TabSharedState;

typedef struct {
    int count;
    char lines[HISTORY_CAP][INPUT_CAP];
} SharedHistory;

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
    
    /* Execution Mode fields */
    int tab_id;
    pthread_t thread_id;        /* Used in Thread Mode */
    pid_t process_id;           /* Used in Process Mode */
    int pipe_to_tab[2];         /* Parent -> Child process pipe */
    int pipe_from_tab[2];       /* Child process -> Parent pipe */
    TabSharedState *shared;     /* Shared tab state (cwd, fg pgid, busy) */
    App *app_ref;
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

struct App {
    Display *display;
    Window window;
    GC gc;

    XftFont *font;
    XftDraw *xft_draw;
    Pixmap backbuffer;
    int backbuffer_width;
    int backbuffer_height;

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
    int foreground_tab_index;
    Job jobs[MAX_JOBS];
    int next_job_id;
    MultiWatch multiwatch;

    /* Execution Mode Flags and Synchronization */
    int use_process_mode;          /* 0 = Thread Mode (default), 1 = Process Mode */
    pthread_mutex_t history_mutex; /* Mutex for Thread Mode */
    sem_t *history_sem;            /* POSIX Semaphore for Process Mode */
    int shm_fd;                    /* Shared Memory File Descriptor */
    SharedHistory *shared_history; /* Shared command history buffer */
    TabSharedState *tab_shared_pool; /* Shared tab state pool */
    pthread_t gui_thread_id;       /* ID of the GUI main thread */

    /* Ctrl+R searchable-history state */
    int history_search_mode;
    char history_search[INPUT_CAP];

    /* Tab-completion candidate state */
    char **completion_matches;
    int completion_count;
    int completion_active;
    int completion_selected;
    size_t completion_start;
    size_t completion_end;

    /* X11 CLIPBOARD selection owned by this application */
    char *clipboard_text;
    Atom clipboard_atom;
    Atom utf8_string_atom;
    Atom targets_atom;
    Atom paste_property;

    /* Mouse selection over rendered terminal output (visual wrapped lines). */
    int selection_active;
    int selecting;
    int selection_anchor_line;
    int selection_anchor_col;
    int selection_end_line;
    int selection_end_col;
};

void gui_run(App *app);
void gui_redraw(App *app);
void shell_execute(App *app, Tab *tab, const char *line);
int builtin_execute(App *app, Tab *tab, const char *line);
int parser_execute_pipeline(App *app, Tab *tab, const char *line);
int job_control_builtin(App *app, Tab *tab, const char *line);
void jobs_poll(App *app);
int hotkey_handle(App *app, XKeyEvent *event, KeySym key, const char *text, int text_len);
void hotkeys_cleanup(App *app);
void tab_append(Tab *tab, const char *text);
void tab_append_raw(Tab *tab, const char *text);
void tab_clear(Tab *tab);
void tab_worker_loop(App *app, Tab *tab);
void history_add(App *app, const char *line);
void history_load(App *app);
void history_save(App *app);
void history_free(App *app);
int multiwatch_start(App *app, Tab *tab, const char *arguments);
void multiwatch_poll(App *app);
void multiwatch_stop(App *app);
void create_tab(App *app);
void close_tab(App *app, int index);
void gui_copy_selection(App *app);

#endif
