#include "terminator.h"
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#define LEFT_MARGIN 12
#define TOP_MARGIN 24
#define LINE_GAP 3

typedef struct {
    char *text;
    int length;
} DisplayLine;

static int font_height(const App *app)
{
    return app->font ? app->font->ascent + app->font->descent : 18;
}

static int line_height(const App *app)
{
    return font_height(app) + LINE_GAP;
}

static int text_width(const App *app, const char *text, int length)
{
    if (app->font != NULL)
        return XTextWidth(app->font, text, length);
    return length * 8;
}

static void free_lines(DisplayLine *lines, size_t count)
{
    for (size_t i = 0; i < count; i++)
        free(lines[i].text);
    free(lines);
}

static int add_display_line(DisplayLine **lines, size_t *count,
                            size_t *capacity, const char *text, size_t length)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 64 : *capacity * 2;
        DisplayLine *new_lines = realloc(*lines, new_capacity * sizeof(**lines));
        if (new_lines == NULL)
            return -1;
        *lines = new_lines;
        *capacity = new_capacity;
    }

    char *copy = malloc(length + 1);
    if (copy == NULL)
        return -1;
    memcpy(copy, text, length);
    copy[length] = '\0';
    (*lines)[*count].text = copy;
    (*lines)[*count].length = (int)length;
    (*count)++;
    return 0;
}

/* Convert text into display rows using the actual font width. */
static int wrap_text(App *app, DisplayLine **lines, size_t *count,
                     size_t *capacity, const char *text, size_t length,
                     int available_width)
{
    size_t start = 0;
    if (length == 0)
        return add_display_line(lines, count, capacity, "", 0);

    while (start < length) {
        size_t end = start;
        size_t last_space = (size_t)-1;
        while (end < length) {
            if (text[end] == ' ' || text[end] == '\t')
                last_space = end;
            if (text_width(app, text + start, (int)(end - start + 1)) > available_width)
                break;
            end++;
        }
        if (end == start)
            end++;
        else if (end < length && last_space != (size_t)-1 && last_space > start)
            end = last_space;

        if (add_display_line(lines, count, capacity, text + start, end - start) != 0)
            return -1;
        start = end;
        while (start < length && (text[start] == ' ' || text[start] == '\t'))
            start++;
    }
    return 0;
}

static int wrap_output(App *app, DisplayLine **lines, size_t *count,
                       size_t *capacity, int available_width)
{
    const char *output = app->tabs[app->active_tab].output;
    size_t length = strlen(output);
    size_t start = 0;

    while (start < length) {
        const char *newline = memchr(output + start, '\n', length - start);
        size_t line_length = newline ? (size_t)(newline - (output + start)) : length - start;
        if (wrap_text(app, lines, count, capacity, output + start,
                      line_length, available_width) != 0)
            return -1;
        start += line_length;
        if (start < length && output[start] == '\n')
            start++;
    }
    return 0;
}

void gui_redraw(App *app)
{
    if (app->tab_count <= 0)
        return;

    Tab *tab = &app->tabs[app->active_tab];
    XClearWindow(app->display, app->window);

    char tabs_line[512] = "";
    for (int i = 0; i < app->tab_count; i++) {
        char label[48];
        const char *base = strrchr(app->tabs[i].cwd, '/');
        base = base != NULL ? base + 1 : app->tabs[i].cwd;
        if (*base == '\0')
            base = "/";
        snprintf(label, sizeof(label), "%s[%d:%.24s] ",
                 i == app->active_tab ? "*" : " ", i + 1, base);
        strncat(tabs_line, label, sizeof(tabs_line) - strlen(tabs_line) - 1);
    }
    XDrawString(app->display, app->window, app->gc, LEFT_MARGIN, TOP_MARGIN - 5,
                tabs_line, (int)strlen(tabs_line));
    XDrawLine(app->display, app->window, app->gc, 0, TOP_MARGIN + 3,
              app->width, TOP_MARGIN + 3);

    int available_width = app->width - LEFT_MARGIN * 2;
    if (available_width < 1)
        available_width = 1;

    DisplayLine *lines = NULL;
    size_t count = 0, capacity = 0;
    if (wrap_output(app, &lines, &count, &capacity, available_width) != 0) {
        free_lines(lines, count);
        XFlush(app->display);
        return;
    }

    char prompt[PATH_MAX + 64];
    if (tab->continuing)
        snprintf(prompt, sizeof(prompt), "> ");
    else
        snprintf(prompt, sizeof(prompt), "user@terminator:%s$ ", tab->cwd);

    char input_line[PATH_MAX + INPUT_CAP + 128];
    snprintf(input_line, sizeof(input_line), "%s%s", prompt, tab->input);
    size_t prompt_length = strlen(prompt);
    size_t cursor_position = prompt_length + tab->cursor;
    size_t input_start_index = count;
    if (wrap_text(app, &lines, &count, &capacity, input_line,
                  strlen(input_line), available_width) != 0) {
        free_lines(lines, count);
        XFlush(app->display);
        return;
    }

    int first_y = TOP_MARGIN + font_height(app) + 10;
    int rows_available = (app->height - first_y - 4) / line_height(app);
    if (rows_available < 1)
        rows_available = 1;
    size_t first_visible = count > (size_t)rows_available
                               ? count - (size_t)rows_available : 0;

    int y = first_y;
    for (size_t i = first_visible; i < count; i++) {
        XDrawString(app->display, app->window, app->gc, LEFT_MARGIN, y,
                    lines[i].text, lines[i].length);

        /* Draw the cursor only when its input row is currently visible. */
        if (i >= input_start_index) {
            size_t position = 0;
            for (size_t j = input_start_index; j < i; j++)
                position += (size_t)lines[j].length;
            if (cursor_position >= position &&
                cursor_position <= position + (size_t)lines[i].length) {
                size_t in_line = cursor_position - position;
                int cursor_x = LEFT_MARGIN + text_width(app, lines[i].text, (int)in_line);
                XDrawLine(app->display, app->window, app->gc, cursor_x,
                          y - font_height(app) + 4, cursor_x, y + 4);
            }
        }
        y += line_height(app);
    }

    free_lines(lines, count);
    XFlush(app->display);
}

void gui_run(App *app)
{
    int x_fd = ConnectionNumber(app->display);

    while (app->running) {
        jobs_poll(app);
        gui_redraw(app);

        if (!XPending(app->display)) {
            struct pollfd descriptor;
            descriptor.fd = x_fd;
            descriptor.events = POLLIN;
            descriptor.revents = 0;
            (void)poll(&descriptor, 1, 100);
            jobs_poll(app);
            gui_redraw(app);
        }

        while (XPending(app->display)) {
            XEvent event;
            XNextEvent(app->display, &event);
            if (event.type == Expose) {
                gui_redraw(app);
            } else if (event.type == ConfigureNotify) {
                app->width = event.xconfigure.width;
                app->height = event.xconfigure.height;
                gui_redraw(app);
            } else if (event.type == KeyPress) {
                char text[32] = "";
                KeySym key;
                int length = XLookupString(&event.xkey, text,
                                           sizeof(text) - 1, &key, NULL);
                text[length] = '\0';
                hotkey_handle(app, &event.xkey, key, text, length);
                gui_redraw(app);
            }
        }
    }
}
