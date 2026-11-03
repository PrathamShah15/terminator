#include "terminator.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <X11/Xatom.h>
#include <ctype.h>



static void clear_completions(App *app)
{
    for (int i = 0; i < app->completion_count; i++)
        free(app->completion_matches[i]);
    free(app->completion_matches);
    app->completion_matches = NULL;
    app->completion_count = 0;
    app->completion_active = 0;
}

static void replace_input_range(Tab *tab, size_t start, size_t end,
                                const char *replacement)
{
    size_t old_length = strlen(tab->input);
    size_t replacement_length = strlen(replacement);
    if (start > end || end > old_length ||
        old_length - (end - start) + replacement_length >= sizeof(tab->input))
        return;

    memmove(tab->input + start + replacement_length, tab->input + end,
            old_length - end + 1);
    memcpy(tab->input + start, replacement, replacement_length);
    tab->cursor = start + replacement_length;
}



static void filename_completion(App *app, Tab *tab)
{
    clear_completions(app);

    size_t cursor = tab->cursor;
    size_t start = cursor;
    while (start > 0 && !isspace((unsigned char)tab->input[start - 1]) &&
           tab->input[start - 1] != '|' && tab->input[start - 1] != '<' &&
           tab->input[start - 1] != '>')
        start--;

    char typed[PATH_MAX];
    size_t typed_length = cursor - start;
    if (typed_length == 0 || typed_length >= sizeof(typed))
        return;
    memcpy(typed, tab->input + start, typed_length);
    typed[typed_length] = '\0';

    char directory_part[PATH_MAX] = "";
    const char *base = typed;
    const char *slash = strrchr(typed, '/');
    if (slash != NULL) {
        size_t dir_length = (size_t)(slash - typed + 1);
        if (dir_length >= sizeof(directory_part)) return;
        memcpy(directory_part, typed, dir_length);
        directory_part[dir_length] = '\0';
        base = slash + 1;
    }

    char search_directory[PATH_MAX];
    if (directory_part[0] == '/') {
        size_t dir_len = strlen(directory_part);

        if (dir_len >= sizeof(search_directory))
            return;

        memcpy(search_directory, directory_part, dir_len + 1);
    } else {
        const char *cwd = tab->cwd[0] ? tab->cwd : ".";

        size_t cwd_len = strlen(cwd);
        size_t dir_len = strlen(directory_part);

        if (cwd_len + 1 + dir_len >= sizeof(search_directory))
            return;

        memcpy(search_directory, cwd, cwd_len);
        search_directory[cwd_len] = '/';

        memcpy(search_directory + cwd_len + 1,
               directory_part, dir_len + 1);
    }
    /* opendir() accepts a trailing slash and ".". */
    DIR *directory = opendir(search_directory);
    if (directory == NULL)
        return;

    size_t base_length = strlen(base);
    struct dirent *entry;
    char **matches = calloc(MAX_COMPLETIONS, sizeof(*matches));
    if (matches == NULL) {
        closedir(directory);
        return;
    }

    int count = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, base, base_length) != 0)
            continue;
        if (entry->d_name[0] == '.' && (base_length == 0 || base[0] != '.'))
            continue;
        if (count >= MAX_COMPLETIONS)
            break;

        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%s%s",
                               directory_part, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(candidate))
            continue;
        matches[count] = strdup(candidate);
        if (matches[count] != NULL)
            count++;
    }
    closedir(directory);

    if (count == 0) {
        free(matches);
        return;
    }

    app->completion_matches = matches;
    app->completion_count = count;
    app->completion_start = start;
    app->completion_end = cursor;

    app->completion_active = 1;
    app->completion_selected = 0;
}

static int longest_common_substring_length(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    if (n == 0 || m == 0) return 0;
    int *previous = calloc(m + 1, sizeof(*previous));
    int *current = calloc(m + 1, sizeof(*current));
    if (previous == NULL || current == NULL) {
        free(previous);
        free(current);
        return 0;
    }

    int best = 0;
    for (size_t i = 1; i <= n; i++) {
        memset(current, 0, (m + 1) * sizeof(*current));
        for (size_t j = 1; j <= m; j++) {
            if (a[i - 1] == b[j - 1]) {
                current[j] = previous[j - 1] + 1;
                if (current[j] > best) best = current[j];
            }
        }
        int *tmp = previous;
        previous = current;
        current = tmp;
    }
    free(previous);
    free(current);
    return best;
}

static void execute_history_search(App *app, Tab *tab)
{
    const char *term = app->history_search;
    int exact = -1;
    for (int i = app->history_count - 1; i >= 0; i--) {
        if (strcmp(app->history[i], term) == 0) {
            exact = i;
            break;
        }
    }

    if (exact >= 0) {
        tab_append(tab, app->history[exact]);
        tab_append(tab, "\n");
        return;
    }

    int best = 0;
    for (int i = 0; i < app->history_count; i++) {
        int length = longest_common_substring_length(app->history[i], term);
        if (length > best) best = length;
    }

    if (best <= 2) {
        tab_append(tab, "No match for search term in history\n");
        return;
    }

    for (int i = 0; i < app->history_count; i++) {
        if (longest_common_substring_length(app->history[i], term) == best) {
            tab_append(tab, app->history[i]);
            tab_append(tab, "\n");
        }
    }
}

static void begin_clipboard_copy(App *app, Tab *tab)
{
    /* Copy highlighted terminal output. Do not silently copy the prompt input. */
    (void)tab;
    if (app->selection_active)
        gui_copy_selection(app);
}

static void begin_clipboard_paste(App *app)
{
    XConvertSelection(app->display, app->clipboard_atom,
                      app->utf8_string_atom, app->paste_property,
                      app->window, CurrentTime);
    XFlush(app->display);
}


void hotkeys_cleanup(App *app)
{
    if (app == NULL) return;
    clear_completions(app);
    free(app->clipboard_text);
    app->clipboard_text = NULL;
}

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
    /* A trailing backslash joins the next line with no separator.
       An open quote keeps a real newline inside the argument. */
    if (!backslash_continuation)
        tab->multiline[used++] = '\n';
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

    int shift = (event->state & ShiftMask) != 0;

    if (control && shift && (key == XK_c || key == XK_C)) {
        begin_clipboard_copy(app, tab);
        return 1;
    }
    if (control && shift && (key == XK_v || key == XK_V)) {
        begin_clipboard_paste(app);
        return 1;
    }

    if (app->completion_active) {
        if (key == XK_Tab) {
            app->completion_selected =
                (app->completion_selected + 1) % app->completion_count;
            return 1;
        }
        if (key == XK_Return || key == XK_KP_Enter) {
            int selected = app->completion_selected;
            if (selected >= 0 && selected < app->completion_count)
                replace_input_range(tab, app->completion_start,
                                    app->completion_end,
                                    app->completion_matches[selected]);
            clear_completions(app);
            return 1; /* First Enter accepts; a second Enter executes. */
        }
        if (key == XK_Escape) {
            clear_completions(app);
            return 1;
        }
        clear_completions(app);
    }

    if (app->history_search_mode) {
        if (key == XK_Return || key == XK_KP_Enter) {
            tab_append(tab, app->history_search);
            tab_append(tab, "\n");
            execute_history_search(app, tab);
            app->history_search_mode = 0;
            app->history_search[0] = '\0';
            return 1;
        }
        if (key == XK_BackSpace) {
            size_t n = strlen(app->history_search);
            if (n > 0) app->history_search[n - 1] = '\0';
            return 1;
        }
        if (key == XK_Escape) {
            app->history_search_mode = 0;
            app->history_search[0] = '\0';
            return 1;
        }
        if (!control && !alt && text != NULL && text_len > 0 &&
            (unsigned char)text[0] >= 32) {
            size_t n = strlen(app->history_search);
            if (n + (size_t)text_len < sizeof(app->history_search))
                memcpy(app->history_search + n, text, (size_t)text_len),
                app->history_search[n + (size_t)text_len] = '\0';
            return 1;
        }
        return 1;
    }

    if (control && key == XK_r) {
        app->history_search_mode = 1;
        app->history_search[0] = '\0';
        return 1;
    }

    if (key == XK_Tab) {
        filename_completion(app, tab);
        return 1;
    }

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
        if (app->foreground_pgid > 0) {
            tab_append(tab, "\nterminator: wait for the foreground command to finish first\n");
            return 1;
        }
        tab->scroll_offset = 0;
        char prompt[PATH_MAX + 64];

        if (tab->continuing) {
            snprintf(prompt, sizeof(prompt), "> ");
            append_visible_command(tab, prompt, tab->input);

            char candidate[INPUT_CAP * 2];
            snprintf(candidate, sizeof(candidate), "%s%s", tab->multiline, tab->input);
            if (command_is_incomplete(candidate)) {
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

        snprintf(prompt, sizeof(prompt), "pratham@terminator:%s$ ", tab->cwd);
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
