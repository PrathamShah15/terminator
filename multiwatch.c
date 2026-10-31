#include "terminator.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define PIPELINE_MAX MAX_COMMANDS
#define ARG_MAXIMUM MAX_ARGS
#define SEPARATOR "----------------------------------------\n"

typedef struct {
    char *argv[ARG_MAXIMUM];
    int argc;
} WatchStage;

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void free_stages(WatchStage stages[], int count)
{
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < stages[i].argc; ++j)
            free(stages[i].argv[j]);
}

static void normalize_compact_example(char *command)
{
    /* Accept the compact spellings in the assignment example too:
       date+%T -> date +%T, and wc-l -> wc -l. */
    if (strncmp(command, "date+", 5) == 0) {
        size_t length = strlen(command);
        if (length + 1 < INPUT_CAP) {
            memmove(command + 5, command + 4, length - 3);
            command[4] = ' ';
        }
    }
    char *p = command;
    while ((p = strstr(p, "wc-")) != NULL) {
        if (p == command || p[-1] == '|' || isspace((unsigned char)p[-1])) {
            memmove(p + 3, p + 2, strlen(p + 2) + 1);
            p[2] = ' ';
            p += 3;
        } else {
            p += 3;
        }
    }
}

static int split_watch_list(const char *input, char commands[][INPUT_CAP], int *count)
{
    const char *p = input;
    *count = 0;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '[') return -1;

    while (1) {
        while (isspace((unsigned char)*p)) ++p;
        if (*p == ']') { ++p; break; }
        if (*p != '"' && *p != '\'') return -1;
        char quote = *p++;
        size_t used = 0;
        while (*p && *p != quote) {
            char c = *p++;
            if (c == '\\' && *p) c = *p++;
            if (used + 1 >= INPUT_CAP) return -1;
            commands[*count][used++] = c;
        }
        if (*p != quote) return -1;
        ++p;
        commands[*count][used] = '\0';
        if (used == 0 || *count >= MAX_WATCH_COMMANDS) return -1;
        normalize_compact_example(commands[*count]);
        ++*count;
        while (isspace((unsigned char)*p)) ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == ']') { ++p; break; }
        return -1;
    }
    while (isspace((unsigned char)*p)) ++p;
    return (*p == '\0' && *count > 0) ? 0 : -1;
}

static int split_pipeline_text(const char *line, char parts[][INPUT_CAP], int *count)
{
    char quote = '\0';
    int escaped = 0;
    size_t used = 0;
    *count = 1;
    memset(parts, 0, sizeof(char) * PIPELINE_MAX * INPUT_CAP);
    for (size_t i = 0; line[i]; ++i) {
        char c = line[i];
        if (escaped) escaped = 0;
        else if (c == '\\' && quote != '\'') escaped = 1;
        else if ((c == '\'' || c == '"') && (quote == '\0' || quote == c))
            quote = quote == '\0' ? c : '\0';
        if (c == '|' && quote == '\0' && !escaped) {
            parts[*count - 1][used] = '\0';
            if (*count >= PIPELINE_MAX) return -1;
            ++*count; used = 0; continue;
        }
        if (used + 1 >= INPUT_CAP) return -1;
        parts[*count - 1][used++] = c;
    }
    parts[*count - 1][used] = '\0';
    return quote == '\0' ? 0 : -1;
}

static int parse_stage(const char *text, WatchStage *stage)
{
    size_t i = 0;
    while (text[i]) {
        while (isspace((unsigned char)text[i])) ++i;
        if (!text[i]) break;
        char word[INPUT_CAP]; size_t used = 0; char quote = '\0';
        while (text[i]) {
            char c = text[i];
            if (!quote && isspace((unsigned char)c)) break;
            ++i;
            if (c == '\\' && quote != '\'' && text[i]) c = text[i++];
            else if ((c == '\'' || c == '"') && (!quote || quote == c)) {
                quote = quote ? '\0' : c;
                continue;
            }
            if (used + 1 < sizeof(word)) word[used++] = c;
        }
        if (quote) return -1;
        word[used] = '\0';
        if (used) {
            if (stage->argc >= ARG_MAXIMUM - 1) return -1;
            stage->argv[stage->argc] = strdup(word);
            if (!stage->argv[stage->argc++]) return -1;
        }
    }
    return stage->argc ? 0 : -1;
}

static void emit_header(App *app, WatchCommand *watch)
{
    char line[INPUT_CAP + 128];
    time_t now = time(NULL);
    snprintf(line, sizeof(line), "\"%s\", %lld:\n%s", watch->command,
             (long long)now, SEPARATOR);
    tab_append(&app->tabs[app->multiwatch.tab_index], line);
    watch->header_printed = 1;
}

static int launch_watch_command(App *app, WatchCommand *watch)
{
    char parts[PIPELINE_MAX][INPUT_CAP];
    int count = 0;
    WatchStage stages[PIPELINE_MAX];
    memset(stages, 0, sizeof(stages));
    if (split_pipeline_text(watch->command, parts, &count) != 0) return -1;
    for (int i = 0; i < count; ++i) {
        if (parse_stage(parts[i], &stages[i]) != 0) {
            free_stages(stages, count);
            return -1;
        }
        stages[i].argv[stages[i].argc] = NULL;
    }

    int pipes[PIPELINE_MAX - 1][2];
    int output_pipe[2];
    for (int i = 0; i < count - 1; ++i) {
        if (pipe(pipes[i]) != 0) {
            for (int j = 0; j < i; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
            free_stages(stages, count); return -1;
        }
    }
    if (pipe(output_pipe) != 0) {
        for (int i = 0; i < count - 1; ++i) { close(pipes[i][0]); close(pipes[i][1]); }
        free_stages(stages, count); return -1;
    }

    watch->pgid = 0; watch->pid_count = 0;
    for (int i = 0; i < count; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            pid_t group = watch->pgid;
            if (group == 0) (void)setpgid(0, 0); else (void)setpgid(0, group);
            if (watch->cwd[0] != '\0') (void)chdir(watch->cwd);
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < count - 1) dup2(pipes[i][1], STDOUT_FILENO);
            else dup2(output_pipe[1], STDOUT_FILENO);
            dup2(output_pipe[1], STDERR_FILENO);
            for (int j = 0; j < count - 1; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
            close(output_pipe[0]); close(output_pipe[1]);
            execvp(stages[i].argv[0], stages[i].argv);
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n", stages[i].argv[0]);
            _exit(127);
        }
        if (pid < 0) {
            if (watch->pgid > 0) (void)kill(-watch->pgid, SIGTERM);
            for (int j = 0; j < i; ++j) (void)waitpid(watch->pids[j], NULL, 0);
            for (int j = 0; j < count - 1; ++j) { close(pipes[j][0]); close(pipes[j][1]); }
            close(output_pipe[0]); close(output_pipe[1]); free_stages(stages, count); return -1;
        }
        if (watch->pgid == 0) watch->pgid = pid;
        (void)setpgid(pid, watch->pgid);
        watch->pids[watch->pid_count++] = pid;
    }
    for (int i = 0; i < count - 1; ++i) { close(pipes[i][0]); close(pipes[i][1]); }
    close(output_pipe[1]);
    int flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    watch->output_fd = output_pipe[0];
    watch->running = 1;
    watch->header_printed = 0;
    emit_header(app, watch);
    free_stages(stages, count);
    return 0;
}

int multiwatch_start(App *app, Tab *tab, const char *arguments)
{
    if (app->multiwatch.active) {
        tab_append(tab, "multiWatch: already running; press Ctrl+C to stop it first\n");
        return 1;
    }
    char commands[MAX_WATCH_COMMANDS][INPUT_CAP];
    int count = 0;
    if (split_watch_list(arguments, commands, &count) != 0) {
        tab_append(tab, "Usage: multiWatch [\"date +%T\", \"ls | wc -l\"]\n");
        return 1;
    }
    memset(&app->multiwatch, 0, sizeof(app->multiwatch));
    app->multiwatch.active = 1;
    app->multiwatch.tab_index = app->active_tab;
    app->multiwatch.count = count;
    for (int i = 0; i < count; ++i)
        app->multiwatch.commands[i].output_fd = -1;
    for (int i = 0; i < count; ++i) {
        WatchCommand *watch = &app->multiwatch.commands[i];
        snprintf(watch->command, sizeof(watch->command), "%s", commands[i]);
        snprintf(watch->cwd, sizeof(watch->cwd), "%s", tab->cwd);
        watch->output_fd = -1;
        watch->active = 1;
        if (launch_watch_command(app, watch) != 0) {
            char message[INPUT_CAP + 80];
            snprintf(message, sizeof(message), "multiWatch: could not start command: %s\n", watch->command);
            tab_append(tab, message);
            multiwatch_stop(app);
            return 1;
        }
    }
    return 1;
}

void multiwatch_poll(App *app)
{
    if (!app->multiwatch.active || app->multiwatch.tab_index < 0 ||
        app->multiwatch.tab_index >= app->tab_count) return;
    Tab *tab = &app->tabs[app->multiwatch.tab_index];
    long long now_ms = monotonic_ms();
    for (int i = 0; i < app->multiwatch.count; ++i) {
        WatchCommand *watch = &app->multiwatch.commands[i];
        if (!watch->active) continue;
        if (watch->running && watch->output_fd >= 0) {
            char buffer[2048];
            for (;;) {
                ssize_t n = read(watch->output_fd, buffer, sizeof(buffer));
                if (n > 0) {
                    char chunk[2049]; memcpy(chunk, buffer, (size_t)n); chunk[n] = '\0';
                    tab_append(tab, chunk); continue;
                }
                if (n == 0) {
                    close(watch->output_fd); watch->output_fd = -1;
                    tab_append(tab, SEPARATOR); break;
                }
                if (errno == EINTR) continue;
                break;
            }
        }
        for (int j = 0; j < watch->pid_count;) {
            int status;
            pid_t result = waitpid(watch->pids[j], &status, WNOHANG);
            if (result == watch->pids[j] || (result < 0 && errno == ECHILD)) {
                watch->pids[j] = watch->pids[watch->pid_count - 1];
                --watch->pid_count;
            } else ++j;
        }
        if (watch->running && watch->pid_count == 0 && watch->output_fd < 0) {
            watch->running = 0;
            watch->next_run_ms = now_ms + 2000;
        }
        if (!watch->running && now_ms >= watch->next_run_ms) {
            if (launch_watch_command(app, watch) != 0) {
                char message[INPUT_CAP + 80];
                snprintf(message, sizeof(message), "multiWatch: failed to rerun: %s\n", watch->command);
                tab_append(tab, message);
                watch->next_run_ms = now_ms + 2000;
            }
        }
    }
}

void multiwatch_stop(App *app)
{
    if (!app->multiwatch.active) return;
    int tab_index = app->multiwatch.tab_index;
    for (int i = 0; i < app->multiwatch.count; ++i) {
        WatchCommand *watch = &app->multiwatch.commands[i];
        if (watch->pgid > 0 && watch->running) (void)kill(-watch->pgid, SIGTERM);
    }
    for (int i = 0; i < app->multiwatch.count; ++i) {
        WatchCommand *watch = &app->multiwatch.commands[i];
        if (watch->pgid > 0 && watch->running) (void)kill(-watch->pgid, SIGKILL);
        for (int j = 0; j < watch->pid_count; ++j)
            while (waitpid(watch->pids[j], NULL, 0) < 0 && errno == EINTR) {}
        if (watch->output_fd >= 0) close(watch->output_fd);
    }
    memset(&app->multiwatch, 0, sizeof(app->multiwatch));
    if (tab_index >= 0 && tab_index < app->tab_count)
        tab_append(&app->tabs[tab_index], "^C\n");
}
