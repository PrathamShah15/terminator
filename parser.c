#include "terminator.h"
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *input_file;
    char *output_file;
} Command;

static void free_commands(Command commands[], int count)
{
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < commands[i].argc; j++)
            free(commands[i].argv[j]);
        free(commands[i].input_file);
        free(commands[i].output_file);
    }
}

static char *copy_word(const char *start, size_t length)
{
    char *word = malloc(length + 1);
    if (word == NULL)
        return NULL;
    memcpy(word, start, length);
    word[length] = '\0';
    return word;
}

static int add_argument(Command *command, const char *word)
{
    if (command->argc >= MAX_ARGS - 1)
        return -1;

    /* Expand filename patterns in this shell, without calling another shell. */
    if (strpbrk(word, "*?[\"") != NULL && strchr(word, '"') == NULL) {
        glob_t matches;
        memset(&matches, 0, sizeof(matches));
        int result = glob(word, 0, NULL, &matches);
        if (result == 0) {
            for (size_t i = 0; i < matches.gl_pathc; i++) {
                if (command->argc >= MAX_ARGS - 1) {
                    globfree(&matches);
                    return -1;
                }
                command->argv[command->argc] = strdup(matches.gl_pathv[i]);
                if (command->argv[command->argc++] == NULL) {
                    globfree(&matches);
                    return -1;
                }
            }
            globfree(&matches);
            return 0;
        }
        globfree(&matches);
    }

    command->argv[command->argc] = strdup(word);
    if (command->argv[command->argc] == NULL)
        return -1;
    command->argc++;
    return 0;
}

static int append_variable(
    const char *text,
    size_t *i,
    char *word,
    size_t *used,
    size_t capacity
)
{
    size_t start;
    size_t end;
    char name[256];
    const char *value;
    size_t len;

    if (text[*i] != '$')
        return 0;

    (*i)++;

    if (text[*i] == '{') {
        start = ++(*i);

        while (text[*i] != '\0' && text[*i] != '}')
            (*i)++;

        if (text[*i] != '}')
            return -1;

        end = *i;
        (*i)++;
    }
    else {
        start = *i;

        if (!(isalpha((unsigned char)text[*i]) ||
              text[*i] == '_')) {
            if (*used + 1 >= capacity)
                return -1;

            word[(*used)++] = '$';
            return 0;
        }

        while (isalnum((unsigned char)text[*i]) ||
               text[*i] == '_')
            (*i)++;

        end = *i;
    }

    len = end - start;

    if (len == 0 || len >= sizeof(name))
        return -1;

    memcpy(name, text + start, len);
    name[len] = '\0';

    value = getenv(name);

    if (value == NULL)
        value = "";

    len = strlen(value);

    if (*used + len >= capacity)
        return -1;

    memcpy(word + *used, value, len);
    *used += len;

    return 1;
}



/*
 * Command substitution is executed by this parser, not by /bin/sh.
 * The child runs the same pipeline parser in capture mode; its stdout is
 * collected by the caller and substituted into the outer command.
 */
static int parser_execute_pipeline_internal(App *app, Tab *tab,
                                            const char *line,
                                            int capture_mode);

static int capture_command_output(App *app, Tab *tab,
                                  const char *command,
                                  char *output, size_t output_size);

static int append_text(char *output, size_t output_size,
                       size_t *used, const char *text, size_t length)
{
    if (length > output_size - *used - 1)
        return -1;
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return 0;
}

/*
 * Find the closing ')' for a "$(" beginning at start.
 * Quotes and backslash escapes inside the substitution are respected.
 * Nested "$(...)" expressions increase the depth.
 */
static int find_substitution_end(const char *input, size_t start,
                                 size_t *end)
{
    size_t i = start + 2;
    int depth = 1;
    char quote = '\0';
    int escaped = 0;

    while (input[i] != '\0') {
        char c = input[i];

        if (escaped) {
            escaped = 0;
            i++;
            continue;
        }

        if (c == '\\' && quote != '\'') {
            escaped = 1;
            i++;
            continue;
        }

        /* A nested $(...) is active inside double quotes, but not
           inside single quotes. */
        if (c == '$' && input[i + 1] == '(' && quote != '\'') {
            depth++;
            i += 2;
            continue;
        }

        if (quote != '\0') {
            if (c == quote)
                quote = '\0';
            i++;
            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            i++;
            continue;
        }

        if (c == '(') {
            depth++;
            i++;
            continue;
        }

        if (c == ')') {
            depth--;
            if (depth == 0) {
                *end = i;
                return 0;
            }
            i++;
            continue;
        }

        i++;
    }

    return -1;
}

/*
 * Expand command substitutions in one command string.
 * Substitutions inside single quotes and escaped "$(" are left literal.
 * The inner command is recursively handled by parser_execute_pipeline_internal.
 */
static int expand_command_substitutions(App *app, Tab *tab,
                                        const char *input,
                                        char *output, size_t output_size)
{
    size_t i = 0;
    size_t used = 0;
    char quote = '\0';

    if (output_size == 0)
        return -1;
    output[0] = '\0';

    while (input[i] != '\0') {
        char c = input[i];

        if (c == '\\' && quote != '\'') {
            /* Preserve the escape and the following character verbatim.
               This also prevents an escaped "$(" from being expanded. */
            if (input[i + 1] != '\0') {
                if (append_text(output, output_size, &used,
                                input + i, 2) != 0)
                    return -1;
                i += 2;
                continue;
            }
        }

        if (c == '\'' && quote != '"') {
            quote = (quote == '\'') ? '\0' : '\'';
            if (append_text(output, output_size, &used, input + i, 1) != 0)
                return -1;
            i++;
            continue;
        }

        if (c == '"' && quote != '\'') {
            quote = (quote == '"') ? '\0' : '"';
            if (append_text(output, output_size, &used, input + i, 1) != 0)
                return -1;
            i++;
            continue;
        }

        if (quote != '\'' && c == '$' && input[i + 1] == '(') {
            size_t end = 0;
            char inner[INPUT_CAP];
            char captured[INPUT_CAP];

            if (find_substitution_end(input, i, &end) != 0)
                return -1;

            size_t inner_len = end - (i + 2);
            if (inner_len == 0 || inner_len >= sizeof(inner))
                return -1;

            memcpy(inner, input + i + 2, inner_len);
            inner[inner_len] = '\0';

            if (capture_command_output(app, tab, inner,
                                       captured, sizeof(captured)) != 0)
                return -1;

            if (append_text(output, output_size, &used,
                            captured, strlen(captured)) != 0)
                return -1;

            i = end + 1;
            continue;
        }

        if (append_text(output, output_size, &used, input + i, 1) != 0)
            return -1;
        i++;
    }

    return quote == '\0' ? 0 : -1;
}

static int parse_words(const char *text, Command *command)
{
    size_t i = 0;
    while (text[i] != '\0') {
        while (isspace((unsigned char)text[i]))
            i++;
        if (text[i] == '\0')
            break;

        if (text[i] == '<' || text[i] == '>') {
            char symbol = text[i++];
            while (isspace((unsigned char)text[i]))
                i++;
            size_t start = i;
            while (text[i] != '\0' && !isspace((unsigned char)text[i]) &&
                   text[i] != '<' && text[i] != '>')
                i++;
            if (start == i)
                return -1;
            char *filename = copy_word(text + start, i - start);
            if (filename == NULL)
                return -1;
            if (symbol == '<') {
                free(command->input_file);
                command->input_file = filename;
            } else {
                free(command->output_file);
                command->output_file = filename;
            }
            continue;
        }

        char word[INPUT_CAP];
        size_t used = 0;
        char quote = '\0';
        while (text[i] != '\0') {
            char c = text[i];
            if (quote == '\0' && (isspace((unsigned char)c) || c == '<' || c == '>'))
                break;
            i++;
            if (c == '\\' && quote != '\'' && text[i] != '\0') {
                c = text[i++];
            }
            else if ((c == '\'' || c == '"') &&
                    (quote == '\0' || quote == c)) {
                quote = quote == '\0' ? c : '\0';
                continue;
            }
            else if (c == '$' && quote != '\'') {
                i--;

                int result = append_variable(
                    text, &i, word, &used, sizeof(word)
                );

                if (result < 0)
                    return -1;

                if (result > 0)
                    continue;

                continue;
            }

            if (used + 1 < sizeof(word))
                word[used++] = c;
        }
        if (quote != '\0')
            return -1;
        word[used] = '\0';
        if (used > 0 && add_argument(command, word) != 0)
            return -1;
    }
    return 0;
}

static int split_pipeline(const char *line, char parts[][INPUT_CAP], int *count)
{
    char quote = '\0';
    int escaped = 0;
    size_t used = 0;
    *count = 1;
    memset(parts, 0, sizeof(char) * MAX_COMMANDS * INPUT_CAP);

    for (size_t i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        if (escaped) {
            escaped = 0;
        } else if (c == '\\' && quote != '\'') {
            escaped = 1;
        } else if ((c == '\'' || c == '"') && (quote == '\0' || quote == c)) {
            quote = quote == '\0' ? c : '\0';
        }
        if (c == '|' && quote == '\0' && !escaped) {
            parts[*count - 1][used] = '\0';
            if (*count >= MAX_COMMANDS)
                return -1;
            (*count)++;
            used = 0;
            continue;
        }
        if (used + 1 >= INPUT_CAP)
            return -1;
        parts[*count - 1][used++] = c;
    }
    parts[*count - 1][used] = '\0';
    return quote == '\0' ? 0 : -1;
}

static int find_free_job(App *app)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!app->jobs[i].active)
            return i;
    }
    return -1;
}

static void append_job_output(App *app, Job *job)
{
    char buffer[2048];
    for (;;) {
        ssize_t bytes = read(job->output_fd, buffer, sizeof(buffer));
        if (bytes > 0) {
            buffer[bytes] = '\0';
            if (job->tab_index >= 0 && job->tab_index < app->tab_count)
                tab_append(&app->tabs[job->tab_index], buffer);
            continue;
        }
        if (bytes < 0 && errno == EINTR)
            continue;
        break;
    }
}

void jobs_poll(App *app)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *job = &app->jobs[i];
        if (!job->active || job->output_fd < 0)
            continue;

        append_job_output(app, job);
        int status;
        pid_t result;
        do {
            result = waitpid(-job->pgid, &status,
                             WNOHANG | WUNTRACED | WCONTINUED);
            if (result > 0) {
                if (WIFSTOPPED(status)) {
                    job->stopped = 1;
                } else if (WIFCONTINUED(status)) {
                    job->stopped = 0;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    append_job_output(app, job);
                    if (job->output_fd >= 0)
                        close(job->output_fd);
                    job->output_fd = -1;
                    job->active = 0;
                    if (job->tab_index >= 0 && job->tab_index < app->tab_count)
                        tab_append(&app->tabs[job->tab_index], "[job] completed\n");
                    break;
                }
            }
        } while (result > 0);

        if (result < 0 && errno == ECHILD) {
            append_job_output(app, job);
            if (job->output_fd >= 0)
                close(job->output_fd);
            job->output_fd = -1;
            job->active = 0;
            if (job->tab_index >= 0 && job->tab_index < app->tab_count)
                tab_append(&app->tabs[job->tab_index], "[job] completed\n");
        }
    }
}

static void drain_output(App *app, Tab *tab, int fd)
{
    char buffer[2048];
    for (;;) {
        ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes > 0) {
            buffer[bytes] = '\0';
            tab_append(tab, buffer);
            continue;
        }
        if (bytes < 0 && errno == EINTR)
            continue;
        break;
    }
    (void)app;
}

/* Wait without freezing X11: Ctrl+C and Ctrl+Z are delivered to the job's process group. */
static int wait_foreground(App *app, Tab *tab, pid_t pgid, int output_fd,
                           const char *command, int *was_stopped,
                           int capture_mode)
{
    int output_open = 1;
    int group_done = 0;
    int final_status = 0;
    *was_stopped = 0;
    app->foreground_pgid = pgid;
    if (tab->shared)
        tab->shared->foreground_pgid = pgid;
    app->foreground_tab_index = (int)(tab - app->tabs);

    int is_worker = 0;
    if (app->use_process_mode) {
        if (tab->pipe_from_tab[1] >= 0) is_worker = 1;
    } else {
        if (!pthread_equal(pthread_self(), app->gui_thread_id)) is_worker = 1;
    }

    /*
     * Capture mode and worker mode must not access X11.
     * GUI thread is already running the X11 event loop in gui_run().
     */
    if (capture_mode || is_worker || app->display == NULL) {
        while (!group_done && !*was_stopped) {
            struct pollfd pfd;
            pfd.fd = output_open ? output_fd : -1;
            pfd.events = POLLIN | POLLHUP;
            pfd.revents = 0;
            (void)poll(&pfd, 1, 50);

            if (output_open && (pfd.revents & (POLLIN | POLLHUP))) {
                char buffer[2048];
                ssize_t bytes;
                while ((bytes = read(output_fd, buffer, sizeof(buffer))) > 0) {
                    if (capture_mode) {
                        size_t offset = 0;
                        while (offset < (size_t)bytes) {
                            ssize_t written = write(STDOUT_FILENO,
                                                    buffer + offset,
                                                    (size_t)bytes - offset);
                            if (written < 0 && errno == EINTR)
                                continue;
                            if (written <= 0)
                                break;
                            offset += (size_t)written;
                        }
                    } else {
                        char chunk[2049];
                        memcpy(chunk, buffer, (size_t)bytes);
                        chunk[bytes] = '\0';
                        tab_append(tab, chunk);
                    }
                }
                if (bytes == 0)
                    output_open = 0;
            }

            int status;
            pid_t result;
            do {
                result = waitpid(-pgid, &status, WNOHANG | WUNTRACED);
                if (result > 0) {
                    if (WIFSTOPPED(status)) {
                        // (void)kill(-pgid, SIGTERM);
                        *was_stopped = 1;
                        final_status = 128 + WSTOPSIG(status);
                        break;
                    }
                    if (WIFEXITED(status))
                        final_status = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status))
                        final_status = 128 + WTERMSIG(status);
                }
            } while (result < 0 && errno == EINTR);

            if (result < 0 && errno == ECHILD)
                group_done = 1;
        }

        if (output_open) {
            char buffer[2048];
            ssize_t bytes;
            while ((bytes = read(output_fd, buffer, sizeof(buffer))) > 0) {
                if (capture_mode) {
                    size_t offset = 0;
                    while (offset < (size_t)bytes) {
                        ssize_t written = write(STDOUT_FILENO,
                                                buffer + offset,
                                                (size_t)bytes - offset);
                        if (written < 0 && errno == EINTR)
                            continue;
                        if (written <= 0)
                            break;
                        offset += (size_t)written;
                    }
                } else {
                    char chunk[2049];
                    memcpy(chunk, buffer, (size_t)bytes);
                    chunk[bytes] = '\0';
                    tab_append(tab, chunk);
                }
            }
        }

        app->foreground_pgid = 0;
        if (tab->shared)
            tab->shared->foreground_pgid = 0;
        app->foreground_tab_index = -1;

        if (*was_stopped) {
            int slot = find_free_job(app);
            if (slot >= 0) {
                Job *job = &app->jobs[slot];
                job->pgid = pgid;
                job->output_fd = output_fd;
                job->active = 1;
                job->stopped = 1;
                job->tab_index = (tab->tab_id >= 0 && tab->tab_id < app->tab_count) ? tab->tab_id : app->active_tab;
                snprintf(job->command, sizeof(job->command), "%s", command);
                char message[INPUT_CAP + 64];
                snprintf(message, sizeof(message), "[%d]+ Stopped     %s\n", slot + 1, command);
                tab_append(tab, message);
                return final_status;
            }
            (void)kill(-pgid, SIGCONT);
            (void)kill(-pgid, SIGTERM);
        }

        close(output_fd);
        return final_status;
    }

    while (!group_done && !*was_stopped) {
        struct pollfd fds[2];
        fds[0].fd = ConnectionNumber(app->display);
        fds[0].events = POLLIN;
        fds[1].fd = output_open ? output_fd : -1;
        fds[1].events = POLLIN | POLLHUP;
        (void)poll(fds, 2, 50);
        int frame_changed = 0;

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char buffer[2048];
            ssize_t bytes;
            while ((bytes = read(output_fd, buffer, sizeof(buffer))) > 0) {
                /* Do not assume output is a NUL-terminated string. */
                char chunk[2049];
                memcpy(chunk, buffer, (size_t)bytes);
                chunk[bytes] = '\0';
                tab_append(tab, chunk);
                frame_changed = 1;
            }
            if (bytes == 0)
                output_open = 0;
        }

        while (XPending(app->display)) {
            XEvent event;
            XNextEvent(app->display, &event);
            if (event.type == KeyPress) {
                char text[32] = "";
                KeySym key;
                int length = XLookupString(&event.xkey, text,
                                           (int)sizeof(text) - 1, &key, NULL);
                if (length < 0) length = 0;
                text[length] = '\0';
                int is_foreground_tab = app->active_tab == app->foreground_tab_index;
                if (is_foreground_tab && (event.xkey.state & ControlMask) &&
                    (key == XK_c || key == XK_C)) {
                    tab_append(tab, "^C\n");
                    (void)kill(-pgid, SIGINT);
                    (void)kill(pgid, SIGINT);
                } else if (is_foreground_tab && (event.xkey.state & ControlMask) &&
                           (key == XK_z || key == XK_Z)) {
                    tab_append(tab, "^Z\n");
                    (void)kill(-pgid, SIGTSTP);
                    (void)kill(pgid, SIGTSTP);
                } else {
                    /* Keep other tabs interactive while this tab has a foreground job. */
                    (void)hotkey_handle(app, &event.xkey, key, text, length);
                }
                frame_changed = 1;
            } else if (event.type == ButtonPress) {
                if (event.xbutton.y < 42 && event.xbutton.button == Button1) {
                    int x = event.xbutton.x;
                    int tab_index = (x - 6) / 220;
                    int local_x = (x - 6) % 220;
                    if (x >= 6 && tab_index >= 0 && tab_index < app->tab_count) {
                        /* Do not close tabs while a foreground wait holds a Tab pointer. */
                        if (local_x >= 185 && local_x <= 200) {
                            close_tab(app, tab_index);
                        } else {
                            app->active_tab = tab_index;
                        }
                    } else if (x >= app->tab_count * 220 + 7 &&
                               x < app->tab_count * 220 + 55) {
                        create_tab(app);
                    }
                } else if (event.xbutton.button == Button4) {
                    app->tabs[app->active_tab].scroll_offset += 3;
                } else if (event.xbutton.button == Button5) {
                    app->tabs[app->active_tab].scroll_offset -= 3;
                    if (app->tabs[app->active_tab].scroll_offset < 0)
                        app->tabs[app->active_tab].scroll_offset = 0;
                }
                frame_changed = 1;
            } else if (event.type == Expose) {
                frame_changed = 1;
            } else if (event.type == ConfigureNotify) {
                app->width = event.xconfigure.width;
                app->height = event.xconfigure.height;
                frame_changed = 1;
            }
        }

        int status;
        pid_t result;
        do {
            result = waitpid(-pgid, &status, WNOHANG | WUNTRACED);
            if (result > 0) {
                if (WIFSTOPPED(status)) {
                    *was_stopped = 1;
                    final_status = 128 + WSTOPSIG(status);
                    break;
                }
                if (WIFEXITED(status))
                    final_status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    final_status = 128 + WTERMSIG(status);
            }
        } while (result > 0);

        if (result < 0 && errno == ECHILD)
            group_done = 1;
        if (frame_changed)
            gui_redraw(app);
    }

    app->foreground_pgid = 0;
    if (tab->shared)
        tab->shared->foreground_pgid = 0;
    app->foreground_tab_index = -1;
    if (*was_stopped) {
        int slot = find_free_job(app);
        if (slot >= 0) {
            Job *job = &app->jobs[slot];
            job->pgid = pgid;
            job->output_fd = output_fd;
            job->active = 1;
            job->stopped = 1;
            job->tab_index = app->active_tab;
            snprintf(job->command, sizeof(job->command), "%s", command);
            char message[INPUT_CAP + 64];
            snprintf(message, sizeof(message), "[%d]+ Stopped  %s\n", slot + 1, command);
            tab_append(tab, message);
            return final_status;
        }
        (void)kill(-pgid, SIGCONT);
        (void)kill(-pgid, SIGTERM);
    }

    if (output_open)
        drain_output(app, tab, output_fd);
    close(output_fd);
    return final_status;
}


/*
 * Run an inner command through this parser and capture stdout.
 * The child has a private copy of App/Tab after fork(), so GUI state changes
 * made by the inner command cannot modify the parent application's memory.
 */
static int capture_command_output(App *app, Tab *tab,
                                  const char *command,
                                  char *output, size_t output_size)
{
    int pipefd[2];
    pid_t pid;
    size_t used = 0;
    int status = 0;

    if (output_size == 0 || pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipefd[1]);

        int result = parser_execute_pipeline_internal(app, tab, command, 1);
        fflush(stdout);
        _exit(result == 0 ? 0 : result);
    }

    close(pipefd[1]);

    for (;;) {
        char buffer[1024];
        ssize_t bytes = read(pipefd[0], buffer, sizeof(buffer));

        if (bytes > 0) {
            if ((size_t)bytes > output_size - used - 1) {
                close(pipefd[0]);
                (void)kill(pid, SIGTERM);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                    ;
                return -1;
            }
            memcpy(output + used, buffer, (size_t)bytes);
            used += (size_t)bytes;
            continue;
        }

        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0) {
            close(pipefd[0]);
            (void)kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
        break;
    }

    close(pipefd[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    /* Command substitution removes trailing newline characters only. */
    while (used > 0 && output[used - 1] == '\n')
        used--;
    output[used] = '\0';

    return 0;
}


int job_control_builtin(App *app, Tab *tab, const char *line)
{
    char name[16] = "";
    char target[32] = "";
    (void)sscanf(line, "%15s %31s", name, target);

    if (strcmp(name, "jobs") == 0) {
        jobs_poll(app);
        for (int i = 0; i < MAX_JOBS; i++) {
            Job *job = &app->jobs[i];
            if (job->active) {
                char row[INPUT_CAP + 64];
                snprintf(row, sizeof(row), "[%d] %s  %s\n", i + 1,
                         job->stopped ? "Stopped" : "Running", job->command);
                tab_append(tab, row);
            }
        }
        return 1;
    }

    if (strcmp(name, "fg") != 0 && strcmp(name, "bg") != 0)
        return 0;

    int index = -1;
    if (target[0] == '%')
        index = atoi(target + 1) - 1;
    else if (target[0] != '\0')
        index = atoi(target) - 1;
    else {
        for (int i = MAX_JOBS - 1; i >= 0; i--) {
            if (app->jobs[i].active) {
                index = i;
                break;
            }
        }
    }

    if (index < 0 || index >= MAX_JOBS || !app->jobs[index].active) {
        tab_append(tab, "terminator: no such job\n");
        return 1;
    }

    Job *job = &app->jobs[index];
    if (strcmp(name, "bg") == 0) {
        if (kill(-job->pgid, SIGCONT) != 0) {
            tab_append(tab, "terminator: could not continue job\n");
        } else {
            job->stopped = 0;
            char message[INPUT_CAP + 64];
            snprintf(message, sizeof(message), "[%d]+ %s &\n", index + 1, job->command);
            tab_append(tab, message);
        }
        return 1;
    }

    tab_append(tab, job->command);
    tab_append(tab, "\n");
    (void)kill(-job->pgid, SIGCONT);
    job->stopped = 0;
    int fd = job->output_fd;
    pid_t pgid = job->pgid;
    char command[INPUT_CAP];
    snprintf(command, sizeof(command), "%s", job->command);
    job->active = 0;
    job->output_fd = -1;
    int stopped = 0;
    (void)wait_foreground(app, tab, pgid, fd, command, &stopped, 0);
    return 1;
}

static int parser_execute_pipeline_internal(App *app, Tab *tab,
                                            const char *line,
                                            int capture_mode)
{
    char command_line[INPUT_CAP];

    if (expand_command_substitutions(app, tab, line,
                                     command_line, sizeof(command_line)) != 0) {
        if (!capture_mode)
            tab_append(tab, "terminator: command substitution failed or too large\n");
        return 2;
    }

    size_t line_length = strlen(command_line);
    while (line_length > 0 && isspace((unsigned char)command_line[line_length - 1]))
        command_line[--line_length] = '\0';
    int background = line_length > 0 && command_line[line_length - 1] == '&';
    if (background && capture_mode) {
        dprintf(STDERR_FILENO,
                "terminator: background commands are not supported inside $(...)\n");
        return 2;
    }
    if (background) {
        command_line[--line_length] = '\0';
        while (line_length > 0 && isspace((unsigned char)command_line[line_length - 1]))
            command_line[--line_length] = '\0';
    }

    char parts[MAX_COMMANDS][INPUT_CAP];
    int count = 0;
    Command commands[MAX_COMMANDS];
    memset(commands, 0, sizeof(commands));

    if (split_pipeline(command_line, parts, &count) != 0) {
        tab_append(tab, "terminator: syntax error in pipeline\n");
        return 2;
    }
    for (int i = 0; i < count; i++) {
        if (parse_words(parts[i], &commands[i]) != 0 || commands[i].argc == 0) {
            tab_append(tab, "terminator: invalid command or quoting\n");
            free_commands(commands, count);
            return 2;
        }
        commands[i].argv[commands[i].argc] = NULL;
    }

    int pipes[MAX_COMMANDS - 1][2];
    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) != 0) {
            tab_append(tab, "terminator: pipe failed\n");
            free_commands(commands, count);
            return 1;
        }
    }
    int output_pipe[2];
    if (pipe(output_pipe) != 0) {
        tab_append(tab, "terminator: output pipe failed\n");
        for (int i = 0; i < count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free_commands(commands, count);
        return 1;
    }

    pid_t children[MAX_COMMANDS];
    int started = 0;
    pid_t pgid = 0;
    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            if (pgid == 0)
                (void)setpgid(0, 0);
            else
                (void)setpgid(0, pgid);
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < count - 1)
                dup2(pipes[i][1], STDOUT_FILENO);
            else
                dup2(output_pipe[1], STDOUT_FILENO);
            if (!capture_mode)
                dup2(output_pipe[1], STDERR_FILENO);

            for (int j = 0; j < count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(output_pipe[0]);
            close(output_pipe[1]);

            if (i == 0 && background && commands[i].input_file == NULL) {
                int null_fd = open("/dev/null", O_RDONLY);
                if (null_fd >= 0) {
                    (void)dup2(null_fd, STDIN_FILENO);
                    close(null_fd);
                }
            }

            if (commands[i].input_file != NULL) {
                int fd = open(commands[i].input_file, O_RDONLY);
                if (fd < 0) {
                    dprintf(STDERR_FILENO, "terminator: cannot open %s\n", commands[i].input_file);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (commands[i].output_file != NULL) {
                int fd = open(commands[i].output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    dprintf(STDERR_FILENO, "terminator: cannot open %s\n", commands[i].output_file);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            execvp(commands[i].argv[0], commands[i].argv);
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n", commands[i].argv[0]);
            _exit(127);
        }
        if (pid < 0) {
            tab_append(tab, "terminator: fork failed\n");
            break;
        }
        if (pgid == 0)
            pgid = pid;
        (void)setpgid(pid, pgid);
        children[started++] = pid;
    }

    for (int i = 0; i < count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    close(output_pipe[1]);
    int flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);

    int status = 0;
    if (started == 0) {
        close(output_pipe[0]);
        free_commands(commands, count);
        return 1;
    }

    if (background) {
        int slot = find_free_job(app);

        if (slot < 0) {
            tab_append(tab, "terminator: job table is full\n");

            // Terminate the complete pipeline process group.
            if (pgid > 0)
                (void)kill(-pgid, SIGTERM);

            // Close the output pipe.
            close(output_pipe[0]);

            // Reap every child to prevent zombie processes.
            for (int i = 0; i < started; i++) {
                pid_t result;

                do {
                    result = waitpid(children[i], NULL, 0);
                } while (result < 0 && errno == EINTR);
            }

            status = 1;
        }
        else {
            Job *job = &app->jobs[slot];

            job->pgid = pgid;
            job->output_fd = output_pipe[0];
            job->active = 1;
            job->stopped = 0;
            job->tab_index = app->active_tab;

            snprintf(job->command, sizeof(job->command),
                    "%s", command_line);

            char message[INPUT_CAP + 64];

            snprintf(message, sizeof(message), "[%d] %ld\n",
                    slot + 1, (long)pgid);

            tab_append(tab, message);
        }
    }
    else {
        int stopped = 0;
        status = wait_foreground(app, tab, pgid, output_pipe[0],
                                 command_line, &stopped, capture_mode);
    }

    free_commands(commands, count);
    return status;
}



/* Public normal-mode entry point used by shell.c and other modules. */
int parser_execute_pipeline(App *app, Tab *tab, const char *line)
{
    return parser_execute_pipeline_internal(app, tab, line, 0);
}
