#include "terminator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void tab_append(Tab *tab, const char *text)
{
    size_t length = strlen(text);
    if (length >= OUTPUT_CAP - tab->output_len) {
        size_t remove = length + tab->output_len - (OUTPUT_CAP - 1);
        if (remove > tab->output_len)
            remove = tab->output_len;
        memmove(tab->output, tab->output + remove, tab->output_len - remove);
        tab->output_len -= remove;
    }
    memcpy(tab->output + tab->output_len, text, length);
    tab->output_len += length;
    tab->output[tab->output_len] = '\0';
}

void tab_clear(Tab *tab)
{
    tab->output_len = 0;
    tab->output[0] = '\0';
}

void history_add(App *app, const char *line)
{
    if (line == NULL || line[0] == '\0')
        return;
    if (app->history_count > 0 &&
        strcmp(app->history[app->history_count - 1], line) == 0)
        return;

    if (app->history_count == HISTORY_CAP) {
        memmove(app->history, app->history + 1,
                sizeof(app->history[0]) * (HISTORY_CAP - 1));
        app->history_count--;
    }
    snprintf(app->history[app->history_count], INPUT_CAP, "%s", line);
    app->history_count++;
}

/* Built-ins are kept in this file because they change shell state. */
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
        app->exit_requested = 1;
        app->running = 0;
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
        return 1;
    }

    if (strcmp(argument, "jobs") == 0 || strcmp(argument, "fg") == 0 ||
        strcmp(argument, "bg") == 0) {
        *end = saved;
        return job_control_builtin(app, tab, argument);
    }

    if (strcmp(argument, "history") == 0) {
        char row[INPUT_CAP + 32];
        for (int i = 0; i < app->history_count; i++) {
            snprintf(row, sizeof(row), "%4d  %s\n", i + 1, app->history[i]);
            tab_append(tab, row);
        }
        return 1;
    }

    *end = saved;
    return 0;
}
