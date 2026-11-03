#include <semaphore.h>
#include <pthread.h>
#include "terminator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

void tab_append_raw(Tab *tab, const char *text)
{
    if (tab == NULL || text == NULL) return;

    size_t length = strlen(text);
    if (length == 0) return;

    if (length >= OUTPUT_CAP) {
        text += length - (OUTPUT_CAP - 1);
        length = OUTPUT_CAP - 1;
        tab->output_len = 0;
    }
    else if (length + tab->output_len >= OUTPUT_CAP) {
        size_t remove = length + tab->output_len - (OUTPUT_CAP - 1);
        memmove(tab->output, tab->output + remove, tab->output_len - remove);
        tab->output_len -= remove;
    }

    memcpy(tab->output + tab->output_len, text, length);
    tab->output_len += length;
    tab->output[tab->output_len] = '\0';
}

void tab_append(Tab *tab, const char *text)
{
    if (tab == NULL || text == NULL) return;
    App *app = tab->app_ref;
    int is_worker = 0;
    if (app) {
        if (app->use_process_mode) {
            if (tab->pipe_from_tab[1] >= 0)
                is_worker = 1;
        } else {
            if (!pthread_equal(pthread_self(), app->gui_thread_id))
                is_worker = 1;
        }
    }

    if (is_worker && tab->pipe_from_tab[1] >= 0) {
        size_t len = strlen(text);
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(tab->pipe_from_tab[1], text + written, len - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            written += (size_t)n;
        }
        return;
    }

    tab_append_raw(tab, text);
}

void tab_clear(Tab *tab)
{
    tab->output_len = 0;
    tab->output[0] = '\0';
}

static void history_lock(App *app)
{
    if (app->use_process_mode) {
        if (app->history_sem != SEM_FAILED && app->history_sem != NULL)
            sem_wait(app->history_sem);
    } else {
        pthread_mutex_lock(&app->history_mutex);
    }
}

static void history_unlock(App *app)
{
    if (app->use_process_mode) {
        if (app->history_sem != SEM_FAILED && app->history_sem != NULL)
            sem_post(app->history_sem);
    } else {
        pthread_mutex_unlock(&app->history_mutex);
    }
}

static int history_file_path(char *path, size_t size)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        return 0;
    int written = snprintf(path, size, "%s/.terminator_history", home);
    return written >= 0 && (size_t)written < size;
}

static int history_store_memory(App *app, const char *line)
{
    if (!app->shared_history) return 0;
    if (app->shared_history->count > 0 &&
        strcmp(app->shared_history->lines[app->shared_history->count - 1], line) == 0)
        return 1;

    if (app->shared_history->count >= HISTORY_CAP) {
        memmove(&app->shared_history->lines[0], &app->shared_history->lines[1],
                sizeof(app->shared_history->lines[0]) * (HISTORY_CAP - 1));
        app->shared_history->count = HISTORY_CAP - 1;
    }

    snprintf(app->shared_history->lines[app->shared_history->count],
             sizeof(app->shared_history->lines[0]), "%s", line);
    app->shared_history->count++;
    app->history_count = app->shared_history->count;
    return 1;
}

void history_save(App *app)
{
    if (!app->shared_history) return;
    char path[PATH_MAX];
    if (!history_file_path(path, sizeof(path)))
        return;

    history_lock(app);
    FILE *file = fopen(path, "w");
    if (file != NULL) {
        for (int i = 0; i < app->shared_history->count; i++)
            fprintf(file, "%s\n", app->shared_history->lines[i]);
        fclose(file);
    }
    history_unlock(app);
}

void history_load(App *app)
{
    char path[PATH_MAX];
    if (!history_file_path(path, sizeof(path)))
        return;

    FILE *file = fopen(path, "r");
    if (file == NULL)
        return;

    history_lock(app);
    char line[INPUT_CAP];
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\n')
            line[--length] = '\0';
        if (length > 0)
            history_store_memory(app, line);
    }
    fclose(file);
    history_unlock(app);
}

void history_add(App *app, const char *line)
{
    if (line == NULL || line[0] == '\0' || !app->shared_history)
        return;

    history_lock(app);
    if (app->shared_history->count > 0 &&
        strcmp(app->shared_history->lines[app->shared_history->count - 1], line) == 0) {
        history_unlock(app);
        return;
    }

    history_store_memory(app, line);

    char path[PATH_MAX];
    if (history_file_path(path, sizeof(path))) {
        FILE *file = fopen(path, "a");
        if (file != NULL) {
            fprintf(file, "%s\n", line);
            fclose(file);
        }
    }
    history_unlock(app);
}

void history_free(App *app)
{
    if (app->shared_history)
        app->shared_history->count = 0;
    app->history_count = 0;
}

int builtin_execute(App *app, Tab *tab, const char *line)
{
    char command[INPUT_CAP];
    char *argument;
    snprintf(command, sizeof(command), "%s", line);
    argument = command;
    while (*argument == ' ' || *argument == '\t')
        argument++;

    char *end = argument;
    while (*end != '\0' && *end != ' ' && *end != '\t')
        end++;
    char saved = *end;
    *end = '\0';

    if (strcmp(argument, "exit") == 0) {
        if (app->use_process_mode && tab->pipe_from_tab[1] >= 0) {
            _exit(0);
        } else if (!app->use_process_mode && !pthread_equal(pthread_self(), app->gui_thread_id)) {
            pthread_exit(NULL);
        } else {
            app->exit_requested = 1;
            app->running = 0;
        }
        return 1;
    }

    if (strcmp(argument, "clear") == 0) {
        tab_clear(tab);
        return 1;
    }

    if (strcmp(argument, "cd") == 0) {
        *end = saved;
        argument = end;
        while (*argument == ' ' || *argument == '\t')
            argument++;
        if (*argument == '\0')
            argument = getenv("HOME");
        if (argument == NULL || chdir(argument) != 0) {
            char message[512];
            snprintf(message, sizeof(message), "cd: cannot change directory: %.400s\n", argument ? argument : "HOME is not set");
            tab_append(tab, message);
        } else if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
            snprintf(tab->cwd, sizeof(tab->cwd), "/");
        }
        if (tab->shared)
            snprintf(tab->shared->cwd, sizeof(tab->shared->cwd), "%s", tab->cwd);
        return 1;
    }

    if (strcmp(argument, "jobs") == 0 || strcmp(argument, "fg") == 0 ||
        strcmp(argument, "bg") == 0) {
        *end = saved;
        return job_control_builtin(app, tab, argument);
    }

    if (strcmp(argument, "history") == 0) {
        char row[INPUT_CAP + 32];
        history_lock(app);
        int total = app->shared_history ? app->shared_history->count : 0;
        int first = total > 1000 ? total - 1000 : 0;
        for (int i = first; i < total; i++) {
            snprintf(row, sizeof(row), "%4d  %s\n", i + 1, app->shared_history->lines[i]);
            tab_append(tab, row);
        }
        history_unlock(app);
        return 1;
    }

    if (strcmp(argument, "multiWatch") == 0) {
        *end = saved;
        argument = end;
        while (*argument == ' ' || *argument == '\t')
            argument++;
        (void)multiwatch_start(app, tab, argument);
        return 1;
    }

    *end = saved;
    return 0;
}
