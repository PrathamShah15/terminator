#include "terminator.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int command_is_builtin(const char *line)
{
    while (isspace((unsigned char)*line))
        line++;
    return (strncmp(line, "cd", 2) == 0 && (line[2] == '\0' || isspace((unsigned char)line[2]))) ||
           (strncmp(line, "exit", 4) == 0 && (line[4] == '\0' || isspace((unsigned char)line[4]))) ||
           strcmp(line, "history") == 0 || strcmp(line, "clear") == 0 ||
           strncmp(line, "jobs", 4) == 0 || strncmp(line, "fg", 2) == 0 ||
           strncmp(line, "bg", 2) == 0;
}

void shell_execute(App *app, Tab *tab, const char *line)
{
    char saved_cwd[PATH_MAX];
    if (getcwd(saved_cwd, sizeof(saved_cwd)) == NULL)
        saved_cwd[0] = '\0';
    if (tab->cwd[0] != '\0')
        chdir(tab->cwd);

    if (command_is_builtin(line)) {
        builtin_execute(app, tab, line);
    } else {
        parser_execute_pipeline(app, tab, line);
    }

    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL)
        snprintf(tab->cwd, sizeof(tab->cwd), "/");
    if (saved_cwd[0] != '\0')
        chdir(saved_cwd);
}
