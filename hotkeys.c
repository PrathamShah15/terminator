#include "terminator.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>


static int command_is_incomplete(const char *line)
{
    char quote = '\0';
    int escaped = 0;
    int substitutions = 0;

    for (size_t i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }
        if ((c == '\'' || c == '"') && (quote == '\0' || quote == c)) {
            quote = quote == '\0' ? c : '\0';
            continue;
        }
        if (quote != '\'') {
            if (c == '(' && i > 0 && line[i - 1] == '$')
                substitutions++;
            else if (c == ')' && substitutions > 0)
                substitutions--;
        }
    }
    return quote != '\0' || substitutions > 0 ||
           (strlen(line) > 0 && line[strlen(line) - 1] == '\\');
}

static void append_multiline(Tab *tab, const char *line)
{
    size_t used = strlen(tab->multiline);
    size_t length = strlen(line);
    if (used + length + 2 >= sizeof(tab->multiline)) {
        tab->multiline[0] = '\0';
        tab->continuing = 0;
        tab_append(tab, "terminator: multiline command is too long\n");
        return;
    }
    int backslash_continuation = length > 0 && line[length - 1] == '\\';
    if (backslash_continuation)
        length--;
    memcpy(tab->multiline + used, line, length);
    used += length;
    tab->multiline[used++] = backslash_continuation ? ' ' : '\n';
    tab->multiline[used] = '\0';
}

static void replace_input(Tab *tab, const char *text)
{
    size_t length = strlen(text);
    if (length >= sizeof(tab->input))
        length = sizeof(tab->input) - 1;
    memcpy(tab->input, text, length);
    tab->input[length] = '\0';
    tab->cursor = length;
}

static void close_active_tab(App *app)
{
    if (app->tab_count <= 1) {
        app->running = 0;
        return;
    }

    for (int i = app->active_tab; i < app->tab_count - 1; i++) {
        app->tabs[i] = app->tabs[i + 1];
    }

    app->tab_count--;
    if (app->active_tab >= app->tab_count) {
        app->active_tab = app->tab_count - 1;
    }
}

static void append_visible_command(Tab *tab, const char *prompt, const char *line)
{
    tab_append(tab, prompt);
    tab_append(tab, line);
    tab_append(tab, "\n");
}

int hotkey_handle(App *app, XKeyEvent *event, KeySym key, const char *text, int text_len)
{
    Tab *tab = &app->tabs[app->active_tab];
    int control = (event->state & ControlMask) != 0;
    int alt = (event->state & Mod1Mask) != 0;
    (void)text_len;

    if (alt && key >= XK_1 && key <= XK_8) {
        int target = (int)(key - XK_1);
        if (target < app->tab_count)
            app->active_tab = target;
        return 1;
    }
    if (control && key == XK_t) {
        if (app->tab_count < MAX_TABS) {
            int index = app->tab_count++;
            memset(&app->tabs[index], 0, sizeof(app->tabs[index]));
            app->tabs[index].alive = 1;
            app->tabs[index].history_index = -1;
            if (getcwd(app->tabs[index].cwd, sizeof(app->tabs[index].cwd)) == NULL)
                snprintf(app->tabs[index].cwd, sizeof(app->tabs[index].cwd), "/");
            app->active_tab = index;
        }
        return 1;
    }
    if (control && key == XK_l) {
        tab_clear(tab);
        return 1;
    }
    if (control && key == XK_a) {
        tab->cursor = 0;
        return 1;
    }
    if (control && key == XK_e) {
        tab->cursor = strlen(tab->input);
        return 1;
    }
    if (control && key == XK_u) {
        memmove(tab->input, tab->input + tab->cursor, strlen(tab->input + tab->cursor) + 1);
        tab->cursor = 0;
        return 1;
    }
    if (control && key == XK_k) {
        tab->input[tab->cursor] = '\0';
        return 1;
    }
    /* Ctrl+W deletes the previous word, not the current tab. */
    if (control && key == XK_w) {
        size_t cursor = tab->cursor;

        /* First remove whitespace immediately before the cursor. */
        while (cursor > 0 &&
               (tab->input[cursor - 1] == ' ' ||
                tab->input[cursor - 1] == '\t')) {
            cursor--;
        }

        /* Then remove the previous word. */
        while (cursor > 0 &&
               tab->input[cursor - 1] != ' ' &&
               tab->input[cursor - 1] != '\t') {
            cursor--;
        }

        memmove(tab->input + cursor,
                tab->input + tab->cursor,
                strlen(tab->input + tab->cursor) + 1);
        tab->cursor = cursor;
        return 1;
    }
    if (key == XK_Left && tab->cursor > 0) {
        tab->cursor--;
        return 1;
    }
    if (key == XK_Right && tab->cursor < strlen(tab->input)) {
        tab->cursor++;
        return 1;
    }
    if (key == XK_Home || (control && key == XK_a)) {
        tab->cursor = 0;
        return 1;
    }
    if (key == XK_End || (control && key == XK_e)) {
        tab->cursor = strlen(tab->input);
        return 1;
    }
    if (key == XK_Up) {
        if (app->history_count > 0) {
            if (tab->history_index < 0)
                tab->history_index = app->history_count - 1;
            else if (tab->history_index > 0)
                tab->history_index--;
            replace_input(tab, app->history[tab->history_index]);
        }
        return 1;
    }
    if (key == XK_Down) {
        if (tab->history_index >= 0 && tab->history_index < app->history_count - 1) {
            tab->history_index++;
            replace_input(tab, app->history[tab->history_index]);
        } else {
            tab->history_index = -1;
            replace_input(tab, "");
        }
        return 1;
    }
    if (key == XK_BackSpace && tab->cursor > 0) {
        memmove(tab->input + tab->cursor - 1, tab->input + tab->cursor,
                strlen(tab->input + tab->cursor) + 1);
        tab->cursor--;
        return 1;
    }
    if (key == XK_Delete && tab->cursor < strlen(tab->input)) {
        memmove(tab->input + tab->cursor, tab->input + tab->cursor + 1,
                strlen(tab->input + tab->cursor + 1) + 1);
        return 1;
    }
    if (control && key == XK_d && tab->input[0] == '\0') {
        close_active_tab(app);
        return 1;
    }
    if (key == XK_Return || key == XK_KP_Enter) {
        char prompt[PATH_MAX + 64];

        if (tab->continuing) {
            snprintf(prompt, sizeof(prompt), "> ");
            append_visible_command(tab, prompt, tab->input);

            if (command_is_incomplete(tab->input)) {
                append_multiline(tab, tab->input);
                tab->input[0] = '\0';
                tab->cursor = 0;
                return 1;
            }

            append_multiline(tab, tab->input);
            char command[INPUT_CAP];
            snprintf(command, sizeof(command), "%s", tab->multiline);
            size_t length = strlen(command);
            if (length > 0 && command[length - 1] == '\n')
                command[length - 1] = '\0';

            history_add(app, command);
            tab->input[0] = '\0';
            tab->multiline[0] = '\0';
            tab->continuing = 0;
            tab->cursor = 0;
            tab->history_index = -1;
            shell_execute(app, tab, command);
            return 1;
        }

        if (tab->input[0] == '\0') {
            tab_append(tab, "\n");
            return 1;
        }

        snprintf(prompt, sizeof(prompt), "user@terminator:%s$ ", tab->cwd);
        append_visible_command(tab, prompt, tab->input);

        if (command_is_incomplete(tab->input)) {
            append_multiline(tab, tab->input);
            tab->continuing = 1;
            tab->input[0] = '\0';
            tab->cursor = 0;
            return 1;
        }

        char command[INPUT_CAP];
        snprintf(command, sizeof(command), "%s", tab->input);
        history_add(app, command);
        tab->input[0] = '\0';
        tab->multiline[0] = '\0';
        tab->continuing = 0;
        tab->cursor = 0;
        tab->history_index = -1;
        shell_execute(app, tab, command);
        return 1;
    }
    if (!control && !alt && text != NULL && text[0] >= ' ' && text[0] != 127) {
        size_t length = strlen(tab->input);
        if (length + 1 < sizeof(tab->input)) {
            memmove(tab->input + tab->cursor + 1, tab->input + tab->cursor,
                    length - tab->cursor + 1);
            tab->input[tab->cursor++] = text[0];
        }
        return 1;
    }
    return 0;
}
