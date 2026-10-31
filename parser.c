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
            } else if ((c == '\'' || c == '"') && (quote == '\0' || quote == c)) {
                quote = quote == '\0' ? c : '\0';
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
                           const char *command, int *was_stopped)
{
    int output_open = 1;
    int group_done = 0;
    int final_status = 0;
    *was_stopped = 0;
    app->foreground_pgid = pgid;

    while (!group_done && !*was_stopped) {
        struct pollfd fds[2];
        fds[0].fd = ConnectionNumber(app->display);
        fds[0].events = POLLIN;
        fds[1].fd = output_open ? output_fd : -1;
        fds[1].events = POLLIN | POLLHUP;
        (void)poll(fds, 2, 50);

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char buffer[2048];
            ssize_t bytes;
            while ((bytes = read(output_fd, buffer, sizeof(buffer))) > 0) {
                /* Do not assume output is a NUL-terminated string. */
                char chunk[2049];
                memcpy(chunk, buffer, (size_t)bytes);
                chunk[bytes] = '\0';
                tab_append(tab, chunk);
            }
            if (bytes == 0)
                output_open = 0;
        }

        while (XPending(app->display)) {
            XEvent event;
            XNextEvent(app->display, &event);
            if (event.type == KeyPress &&
                (event.xkey.state & ControlMask)) {
                KeySym key;
                char text[8];
                XLookupString(&event.xkey, text, sizeof(text), &key, NULL);
                if (key == XK_c || key == XK_C) {
                    tab_append(tab, "^C\n");
                    (void)kill(-pgid, SIGINT);
                } else if (key == XK_z || key == XK_Z) {
                    tab_append(tab, "^Z\n");
                    (void)kill(-pgid, SIGTSTP);
                }
            } else if (event.type == Expose) {
                gui_redraw(app);
            } else if (event.type == ConfigureNotify) {
                app->width = event.xconfigure.width;
                app->height = event.xconfigure.height;
                gui_redraw(app);
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
        gui_redraw(app);
    }

    app->foreground_pgid = 0;
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

int job_control_builtin(App *app, Tab *tab, const char *line)
{
    char name[16] = "";
    char target[32] = "";
    (void)sscanf(line, "%15s %31s", name, target);

    if (strcmp(name, "jobs") == 0) {
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

    (void)kill(-job->pgid, SIGCONT);
    job->stopped = 0;
    int fd = job->output_fd;
    pid_t pgid = job->pgid;
    char command[INPUT_CAP];
    snprintf(command, sizeof(command), "%s", job->command);
    job->active = 0;
    job->output_fd = -1;
    int stopped = 0;
    (void)wait_foreground(app, tab, pgid, fd, command, &stopped);
    return 1;
}

int parser_execute_pipeline(App *app, Tab *tab, const char *line)
{
    char command_line[INPUT_CAP];
    snprintf(command_line, sizeof(command_line), "%s", line);
    size_t line_length = strlen(command_line);
    while (line_length > 0 && isspace((unsigned char)command_line[line_length - 1]))
        command_line[--line_length] = '\0';
    int background = line_length > 0 && command_line[line_length - 1] == '&';
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
            dup2(output_pipe[1], STDERR_FILENO);

            for (int j = 0; j < count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(output_pipe[0]);
            close(output_pipe[1]);

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
        status = wait_foreground(app,tab,pgid,output_pipe[0],command_line,&stopped);
    }

    free_commands(commands, count);
    return status;
}