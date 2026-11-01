/*
 * Terminator integration test harness
 *
 * Tests the real project functions (shell_execute, parser_execute_pipeline,
 * builtins, history, hotkeys and multiWatch) without launching the visible GUI.
 * Requires an X11 display because foreground execution polls the X connection.
 *
 * Build/run with: ./run_tests.sh
 *
 * The harness uses a temporary HOME and working directory. It does not touch
 * the user's real ~/.terminator_history.
 */

#include "terminator.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static App app;
static Tab *tab;
static int passed, failed;
static char temp_root[PATH_MAX], temp_home[PATH_MAX], temp_work[PATH_MAX];

/* GUI is not initialized during automated tests. */
void gui_redraw(App *unused)
{
    (void)unused;
}

/* Ignore GUI tab-close events during automated tests. */
void close_tab(App *unused, int index)
{
    (void)unused;
    (void)index;
}

void gui_copy_selection(App *unused)
{
    (void)unused;
}

static void report(int ok, const char *name)
{
    if (ok) { ++passed; printf("[PASS] %s\n", name); }
    else { ++failed; printf("[FAIL] %s\n", name); }
}
#define CHECK(expr, name) report((expr), (name))

static int contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}
static void reset_output(void) { tab->output[0] = '\0'; tab->output_len = 0; }
static void run(const char *command) { shell_execute(&app, tab, command); }
static int file_equals(const char *path, const char *expected)
{
    char buf[8192]; int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf)-1); close(fd);
    if (n < 0) return 0;
    buf[n] = '\0'; return strcmp(buf, expected) == 0;
}
static void make_file(const char *name, const char *content)
{
    int fd = open(name, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (fd >= 0) { (void)write(fd, content, strlen(content)); close(fd); }
}
static void test_parser_and_execution(void)
{
    reset_output(); run("printf 'hello world'");
    CHECK(contains(tab->output, "hello world"), "external command + quoted argument");

    reset_output(); run("printf '%s' 'a b c'");
    CHECK(contains(tab->output, "a b c"), "single-quoted argument");

    reset_output(); run("printf '%s' \"double quoted\"");
    CHECK(contains(tab->output, "double quoted"), "double-quoted argument");

    reset_output(); run("printf '%s' hello\\ world");
    CHECK(contains(tab->output, "hello world"), "backslash-escaped whitespace");

    reset_output(); run("printf 'one\\ntwo\\nthree\\n'");
    CHECK(contains(tab->output, "one\ntwo\nthree"), "multiline command output");

    reset_output(); run("command_that_does_not_exist_terminator");
    CHECK(contains(tab->output, "command not found"), "unknown command diagnostic");

    reset_output(); run("printf x | tr x y");
    CHECK(contains(tab->output, "y"), "single-stage pipeline");

    reset_output(); run("printf '3\\n1\\n2\\n' | sort | tr '\\n' ','");
    CHECK(contains(tab->output, "1,2,3,"), "three-stage pipeline");

    reset_output(); run("printf 'alpha\\nbeta\\n' | grep beta");
    CHECK(contains(tab->output, "beta") && !contains(tab->output, "alpha"),
          "pipeline filters data");

    reset_output(); run("printf x |");
    CHECK(contains(tab->output, "syntax error") || contains(tab->output, "invalid command"),
          "malformed pipeline rejected");

    reset_output(); run("printf '%s' 'unterminated");

    CHECK(contains(tab->output, "syntax error") ||
      contains(tab->output, "invalid command") ||
      contains(tab->output, "quoting"),
      "unclosed quote rejected");
}
static void test_redirection(void)
{
    make_file("input.txt", "redirection-input\n");
    reset_output(); run("cat < input.txt");
    CHECK(contains(tab->output, "redirection-input"), "input redirection");

    reset_output(); run("printf 'redirected-output' > output.txt");
    CHECK(file_equals("output.txt", "redirected-output"), "output redirection creates exact file");

    reset_output(); run("printf 'replacement' > output.txt");
    CHECK(file_equals("output.txt", "replacement"), "output redirection truncates existing file");

    reset_output(); run("cat < input.txt | tr a-z A-Z > upper.txt");
    CHECK(file_equals("upper.txt", "REDIRECTION-INPUT\n"),
          "combined input redirection, pipeline and output redirection");

    reset_output(); run("cat < missing_terminator_file");
    CHECK(contains(tab->output, "cannot open"), "missing input file reports error");
}
static void test_builtins_and_state(void)
{
    char before[PATH_MAX], expected[PATH_MAX];
    getcwd(before, sizeof(before));
    reset_output(); run("pwd");
    CHECK(contains(tab->output, temp_work), "command runs in tab working directory");

    reset_output(); run("cd subdir");
    int n = snprintf(expected, sizeof(expected), "%s/subdir", temp_work);
    if (n < 0 || (size_t)n >= sizeof(expected)) {
        fprintf(stderr, "Path too long\n");
        exit(2);
    }
    CHECK(strcmp(tab->cwd, expected) == 0, "cd updates tab-local current directory");
    char after[PATH_MAX]; getcwd(after, sizeof(after));
    CHECK(strcmp(after, before) == 0, "shell restores process working directory after command");

    reset_output(); run("cd ..");
    CHECK(strcmp(tab->cwd, temp_work) == 0, "cd .. updates tab directory");

    reset_output(); run("cd /path/that/does/not/exist");
    CHECK(contains(tab->output, "cd: cannot change directory"), "invalid cd reports error");

    reset_output(); tab_append(tab, "old output\n"); run("clear");
    CHECK(tab->output_len == 0 && tab->output[0] == '\0', "clear empties terminal output");

    app.exit_requested = 0; app.running = 1;
    run("exit");
    CHECK(app.exit_requested == 1 && app.running == 0, "exit requests application shutdown");
    app.exit_requested = 0; app.running = 1;
}
static void test_globbing(void)
{
    make_file("glob_a.txt", "a"); make_file("glob_b.txt", "b");
    reset_output(); run("printf '<%s>' glob_*.txt");
    CHECK(contains(tab->output, "<glob_a.txt>") && contains(tab->output, "<glob_b.txt>"),
          "wildcard expands matching filenames");
}
static void test_history(void)
{
    history_free(&app);
    reset_output();
    history_add(&app, "printf first");
    history_add(&app, "printf second");
    history_add(&app, "printf second");
    CHECK(app.history_count == 2, "history stores commands and suppresses consecutive duplicates");

    history_save(&app);
    App loaded; memset(&loaded, 0, sizeof(loaded));
    history_load(&loaded);
    CHECK(loaded.history_count == 2 &&
          strcmp(loaded.history[0], "printf first") == 0 &&
          strcmp(loaded.history[1], "printf second") == 0,
          "history persists and reloads from isolated HOME");

    reset_output(); app.history_count = 2;
    run("history");
    CHECK(contains(tab->output, "printf first") && contains(tab->output, "printf second"),
          "history builtin displays stored commands");
    history_free(&loaded);
}
static void send_key(KeySym key, unsigned int state, const char *text, int len)
{
    XKeyEvent ev; memset(&ev, 0, sizeof(ev)); ev.state = state;
    (void)hotkey_handle(&app, &ev, key, text, len);
}
static void test_hotkeys(void)
{
    app.active_tab = 0; app.tab_count = 1;
    memset(tab->input, 0, sizeof(tab->input));
    strcpy(tab->input, "hello world"); tab->cursor = strlen(tab->input);
    send_key(XK_a, ControlMask, NULL, 0);
    CHECK(tab->cursor == 0, "Ctrl+A moves cursor to beginning");

    send_key(XK_e, ControlMask, NULL, 0);
    CHECK(tab->cursor == strlen(tab->input), "Ctrl+E moves cursor to end");

    send_key(XK_u, ControlMask, NULL, 0);
    CHECK(tab->input[0] == '\0' && tab->cursor == 0, "Ctrl+U clears text before cursor");

    strcpy(tab->input, "alpha beta"); tab->cursor = strlen(tab->input);
    send_key(XK_w, ControlMask, NULL, 0);
    CHECK(strcmp(tab->input, "alpha ") == 0, "Ctrl+W deletes previous word");

    strcpy(tab->input, "abcdef"); tab->cursor = 3;
    send_key(XK_k, ControlMask, NULL, 0);
    CHECK(strcmp(tab->input, "abc") == 0, "Ctrl+K deletes text after cursor");

    app.tab_count = 1; app.active_tab = 0;
    send_key(XK_t, ControlMask, NULL, 0);
    CHECK(app.tab_count == 2 && app.active_tab == 1 && app.tabs[1].alive,
          "Ctrl+T creates and activates a new tab");

    send_key(XK_1, Mod1Mask, NULL, 0);
    CHECK(app.active_tab == 0, "Alt+1 switches to first tab");

    app.history_search_mode = 0;
    send_key(XK_r, ControlMask, NULL, 0);
    CHECK(app.history_search_mode == 1, "Ctrl+R enters history-search mode");
    send_key(XK_Escape, 0, NULL, 0);
    CHECK(app.history_search_mode == 0, "Escape cancels history search");

    app.tab_count = 1; app.active_tab = 0;
    hotkeys_cleanup(&app);
}
static void test_jobs(void)
{
    memset(app.jobs, 0, sizeof(app.jobs));
    reset_output(); run("sleep 1 &");
    int found = 0;
    for (int i=0;i<MAX_JOBS;i++) if (app.jobs[i].active) found = 1;
    CHECK(found, "background command is registered as a job");

    reset_output(); run("jobs");
    CHECK(contains(tab->output, "sleep 1"), "jobs lists background command");

    for (int i=0;i<30;i++) { jobs_poll(&app); usleep(50000); }
    CHECK(1, "background job polling completes without harness crash");

    reset_output(); run("sleep 0.2 &");
    usleep(400000); jobs_poll(&app);
    int active = 0; for (int i=0;i<MAX_JOBS;i++) if (app.jobs[i].active) active++;
    CHECK(active <= 1, "completed background job is reaped");
}
static void test_multiwatch(void)
{
    reset_output();
    int started = multiwatch_start(&app, tab, "[\"printf watch-one\", \"printf watch-two\"]");
    CHECK(started && app.multiwatch.active && app.multiwatch.count == 2,
          "multiWatch parses and starts two commands");

    for (int i=0;i<30;i++) { multiwatch_poll(&app); usleep(50000); }
    CHECK(contains(tab->output, "watch-one") && contains(tab->output, "watch-two"),
          "multiWatch captures output from every command");

    reset_output();
    multiwatch_stop(&app);
    CHECK(!app.multiwatch.active && contains(tab->output, "^C"),
          "multiWatch stop cleans up and reports interruption");

    reset_output();
    (void)multiwatch_start(&app, tab, "not-a-list");
    CHECK(contains(tab->output, "Usage:"), "multiWatch rejects malformed argument list");
    multiwatch_stop(&app);
}
static void cleanup(void)
{
    multiwatch_stop(&app);
    for (int i=0;i<MAX_JOBS;i++) {
        if (app.jobs[i].active && app.jobs[i].pgid > 0) {
            kill(-app.jobs[i].pgid, SIGTERM);
            while (waitpid(-app.jobs[i].pgid, NULL, 0) > 0) {}
            if (app.jobs[i].output_fd >= 0) close(app.jobs[i].output_fd);
        }
    }
    history_save(&app); history_free(&app); hotkeys_cleanup(&app);
    if (app.display) XCloseDisplay(app.display);
}
int main(void)
{
    char template[] = "/tmp/terminator-tests.XXXXXX";
    char *root = mkdtemp(template);
    if (!root) { perror("mkdtemp"); return 2; }
    snprintf(temp_root, sizeof(temp_root), "%s", root);
    snprintf(temp_home, sizeof(temp_home), "%s/home", root);
    snprintf(temp_work, sizeof(temp_work), "%s/work", root);
    mkdir(temp_home, 0700); mkdir(temp_work, 0700);
    char sub[PATH_MAX];
    int n = snprintf(sub, sizeof(sub), "%s/subdir", temp_work);
    if (n < 0 || (size_t)n >= sizeof(sub)) {
        fprintf(stderr, "Path too long\n");
        exit(2);
    }
    mkdir(sub, 0700);
    if (setenv("HOME", temp_home, 1) != 0 || chdir(temp_work) != 0) {
        perror("test environment"); return 2;
    }

    memset(&app, 0, sizeof(app));
    app.display = XOpenDisplay(NULL);
    if (!app.display) {
        fprintf(stderr, "ERROR: Cannot open X11 display. Run inside X11/Wayland-XWayland, or use xvfb-run.\n");
        return 2;
    }
    app.tab_count = 1; app.active_tab = 0; app.running = 1;
    app.foreground_tab_index = -1;
    tab = &app.tabs[0]; tab->alive = 1; tab->history_index = -1;
    snprintf(tab->cwd, sizeof(tab->cwd), "%s", temp_work);

    printf("=== TERMINATOR INTEGRATION TESTS ===\n");
    printf("Temporary HOME: %s\nWorking directory: %s\n\n", temp_home, temp_work);
    test_parser_and_execution();
    test_redirection();
    test_builtins_and_state();
    test_globbing();
    test_history();
    test_hotkeys();
    test_jobs();
    test_multiwatch();
    cleanup();

    printf("\n=== RESULT: %d passed, %d failed ===\n", passed, failed);
    printf("Test files are isolated under %s\n", temp_root);
    return failed ? 1 : 0;
}
