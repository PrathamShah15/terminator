#include "terminator.h"
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#define LEFT_MARGIN 14
#define TAB_BAR_HEIGHT 42
#define CONTENT_TOP 58
#define LINE_GAP 3
#define TAB_WIDTH 220
#define PLUS_WIDTH 48

typedef struct { char *text; int length; } DisplayLine;

static int font_height(const App *app)
{
    return app->font
        ? app->font->ascent + app->font->descent
        : 18;
}

static int line_height(const App *app)
{
    return font_height(app) + LINE_GAP;
}

static int text_width(const App *app, const char *text, int length)
{
    if (app->font == NULL)
        return length * 8;

    XGlyphInfo extents;

    XftTextExtentsUtf8(app->display,app->font,(const FcChar8 *)text,length,&extents);
    return extents.xOff;
}

static void draw_text(App *app,int x,int y,const char *text,int length,XftColor *color)
{
    if (app->font == NULL || app->xft_draw == NULL)
        return;

    XftDrawStringUtf8(app->xft_draw,color,app->font,x,y,(const FcChar8 *)text,length);
}


static void free_lines(DisplayLine *lines, size_t count)
{
    for (size_t i = 0; i < count; i++) free(lines[i].text);
    free(lines);
}

static int add_display_line(DisplayLine **lines, size_t *count, size_t *capacity, const char *text, size_t length)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 64 : *capacity * 2;
        DisplayLine *new_lines = realloc(*lines, new_capacity * sizeof(**lines));
        if (!new_lines) return -1;
        *lines = new_lines; *capacity = new_capacity;
    }
    char *copy = malloc(length + 1);
    if (!copy) return -1;
    memcpy(copy, text, length); copy[length] = '\0';
    (*lines)[*count].text = copy; (*lines)[*count].length = (int)length; (*count)++;
    return 0;
}

static int wrap_text(App *app, DisplayLine **lines, size_t *count, size_t *capacity,
                     const char *text, size_t length, int available_width)
{
    size_t start = 0;
    if (length == 0) return add_display_line(lines, count, capacity, "", 0);
    while (start < length) {
        size_t end = start, last_space = (size_t)-1;
        while (end < length) {
            if (text[end] == ' ' || text[end] == '\t') last_space = end;
            if (text_width(app, text + start, (int)(end - start + 1)) > available_width) break;
            end++;
        }
        if (end == start) end++;
        else if (end < length && last_space != (size_t)-1 && last_space > start) end = last_space;
        if (add_display_line(lines, count, capacity, text + start, end - start) != 0) return -1;
        start = end;
        while (start < length && (text[start] == ' ' || text[start] == '\t')) start++;
    }
    return 0;
}

static int wrap_output(App *app, DisplayLine **lines, size_t *count, size_t *capacity, int available_width)
{
    const char *output = app->tabs[app->active_tab].output;
    size_t length = app->tabs[app->active_tab].output_len;
    if (length > strlen(output)) length = strlen(output);
    size_t start = 0;
    while (start < length) {
        const char *newline = memchr(output + start, '\n', length - start);
        size_t line_length = newline ? (size_t)(newline - (output + start)) : length - start;
        if (wrap_text(app, lines, count, capacity, output + start, line_length, available_width) != 0) return -1;
        start += line_length;
        if (start < length && output[start] == '\n') start++;
    }
    return 0;
}

static void draw_tab_bar(App *app)
{
    XSetForeground(app->display, app->gc, app->tab_background);
    XFillRectangle(app->display, app->window, app->gc, 0, 0, (unsigned)app->width, TAB_BAR_HEIGHT);
    XSetForeground(app->display, app->gc, app->border_color);
    XDrawLine(app->display, app->window, app->gc, 0, TAB_BAR_HEIGHT - 1, app->width, TAB_BAR_HEIGHT - 1);
    for (int i = 0; i < app->tab_count; i++) {
        int x = i * TAB_WIDTH + 6, width = TAB_WIDTH - 5;
        XSetForeground(app->display, app->gc, i == app->active_tab ? app->active_tab_background : app->tab_background);
        XFillRectangle(app->display, app->window, app->gc, x, 5, (unsigned)width, TAB_BAR_HEIGHT - 11);
        XSetForeground(app->display, app->gc, app->border_color);
        XDrawRectangle(app->display, app->window, app->gc, x, 5, (unsigned)width, TAB_BAR_HEIGHT - 11);
        XSetForeground(app->display, app->gc, i == app->active_tab ? app->accent : app->foreground);
        char label[32]; snprintf(label, sizeof(label), "%d  %s", i + 1, i == app->active_tab ? "Terminal" : "Tab");
        draw_text(app,x+10,27,label,(int)strlen(label),i == app->active_tab
        ? &app->xft_accent: &app->xft_foreground);

        int cx = x + width - 17;

        XftFont *close_font = XftFontOpenName(app->display,DefaultScreen(app->display),
            "DejaVu Sans Mono:style=Bold:size=14");

        if (close_font != NULL) {const char *close_symbol = "×";XGlyphInfo extents;

        XftTextExtentsUtf8(app->display,close_font,(const FcChar8 *)close_symbol,
        strlen(close_symbol),&extents);

        int close_x = (cx - extents.xOff / 2)-5;
        int close_y = 26;

        XftDrawStringUtf8(app->xft_draw,&app->xft_accent,close_font,close_x,
            close_y,(const FcChar8 *)close_symbol,strlen(close_symbol));

        XftFontClose(app->display, close_font);
        }
    }

    int plus_x = app->tab_count * TAB_WIDTH + 7;
    if (plus_x + PLUS_WIDTH < app->width) {
        XSetForeground(app->display, app->gc, app->tab_background);
        XFillRectangle(app->display, app->window, app->gc, plus_x, 5, PLUS_WIDTH - 5, TAB_BAR_HEIGHT - 11);
        XSetForeground(app->display, app->gc, app->accent);
        XDrawRectangle(app->display, app->window, app->gc, plus_x, 5, PLUS_WIDTH - 5, TAB_BAR_HEIGHT - 11);
        int plus_width = text_width(app, "+", 1);
        int text_x = plus_x + (32 - plus_width) / 2 + 6;
        int text_y = 27;

        draw_text(app, text_x, text_y, "+", 1, &app->xft_accent);
    }
}

static void create_tab(App *app)
{
    if (app->tab_count >= MAX_TABS) return;
    int index = app->tab_count++;
    memset(&app->tabs[index], 0, sizeof(app->tabs[index]));
    app->tabs[index].alive = 1; app->tabs[index].history_index = -1;
    if (getcwd(app->tabs[index].cwd, sizeof(app->tabs[index].cwd)) == NULL)
        snprintf(app->tabs[index].cwd, sizeof(app->tabs[index].cwd), "/");
    app->active_tab = index;
}

static void close_tab(App *app, int index)
{
    if (index < 0 || index >= app->tab_count) return;
    if (app->tab_count == 1) { app->running = 0; return; }
    for (int i = index; i < app->tab_count - 1; i++) app->tabs[i] = app->tabs[i + 1];
    app->tab_count--;
    if (app->active_tab > index) app->active_tab--;
    else if (app->active_tab == index && app->active_tab >= app->tab_count) app->active_tab = app->tab_count - 1;
    if (app->active_tab < 0) app->active_tab = 0;
}

void gui_redraw(App *app)
{
    if (app->tab_count <= 0) return;
    if (app->active_tab < 0 || app->active_tab >= app->tab_count) app->active_tab = 0;
    Tab *tab = &app->tabs[app->active_tab];
    XClearWindow(app->display, app->window);
    draw_tab_bar(app);
    int available_width = app->width - LEFT_MARGIN * 2;
    if (available_width < 1) available_width = 1;
    DisplayLine *lines = NULL; size_t count = 0, capacity = 0;
    if (wrap_output(app, &lines, &count, &capacity, available_width) != 0) { free_lines(lines, count); XFlush(app->display); return; }
    char prompt[PATH_MAX + 64];
    if (tab->continuing) snprintf(prompt, sizeof(prompt), "> ");
    else snprintf(prompt, sizeof(prompt), "user@terminator:%s$ ", tab->cwd);
    char input_line[PATH_MAX + INPUT_CAP + 128];
    snprintf(input_line, sizeof(input_line), "%s%s", prompt, tab->input);
    size_t prompt_length = strlen(prompt), cursor_position = prompt_length + tab->cursor;
    size_t input_start_index = count;
    if (wrap_text(app, &lines, &count, &capacity, input_line, strlen(input_line), available_width) != 0) { free_lines(lines, count); XFlush(app->display); return; }

    int first_y = CONTENT_TOP + font_height(app);
    int rows_available = (app->height - first_y - 8) / line_height(app);
    if (rows_available < 1) rows_available = 1;
    size_t input_rows = count - input_start_index;
    if (input_rows > (size_t)rows_available) input_rows = (size_t)rows_available;
    size_t output_rows = (size_t)rows_available - input_rows;
    size_t output_count = input_start_index;
    int max_scroll = output_count > output_rows ? (int)(output_count - output_rows) : 0;
    if (tab->scroll_offset < 0) tab->scroll_offset = 0;
    if (tab->scroll_offset > max_scroll) tab->scroll_offset = max_scroll;
    size_t output_end = output_count - (size_t)tab->scroll_offset;
    size_t output_start = output_end > output_rows ? output_end - output_rows : 0;
    int y = first_y;
    
    for (size_t i = output_start; i < output_end; i++) {
        draw_text(app,LEFT_MARGIN,y,lines[i].text,lines[i].length,&app->xft_foreground);
        y += line_height(app);
    }

    // if (tab->scroll_offset == 0 && output_rows > 0 && output_count > output_rows)
    //     y = first_y + (int)(output_rows - (output_end - output_start)) * line_height(app);
    for (size_t i = input_start_index; i < count; i++) {
        const char *line = lines[i].text; size_t line_length = (size_t)lines[i].length, global_start = 0;
        for (size_t j = input_start_index; j < i; j++) global_start += (size_t)lines[j].length;
        size_t prompt_part = global_start < prompt_length ? prompt_length - global_start : 0;
        if (prompt_part > line_length) prompt_part = line_length;
        int x = LEFT_MARGIN;
        
        if (prompt_part > 0) {
            draw_text(app,x,y,line,(int)prompt_part,&app->xft_prompt_color);
            x += text_width(app, line, (int)prompt_part);
        }

        if (line_length > prompt_part) {
            draw_text(app,x,y,line + prompt_part,(int)(line_length - prompt_part),&app->xft_foreground);
        }

        size_t line_end = global_start + line_length;
        if (cursor_position >= global_start && cursor_position <= line_end) {
            size_t in_line = cursor_position - global_start;
            int cursor_x = LEFT_MARGIN + text_width(app, line, (int)in_line);
            XSetForeground(app->display, app->gc, app->accent);
            XDrawLine(app->display, app->window, app->gc, cursor_x, y - font_height(app) + 4, cursor_x, y + 4);
        }
        y += line_height(app);
    }
    free_lines(lines, count); XFlush(app->display);
}

void gui_run(App *app)
{
    int x_fd = ConnectionNumber(app->display);
    while (app->running) {
        jobs_poll(app);
        multiwatch_poll(app);
        gui_redraw(app);
        if (!XPending(app->display)) {
            struct pollfd descriptor = { .fd = x_fd, .events = POLLIN, .revents = 0 };
            (void)poll(&descriptor, 1, 100);
            jobs_poll(app);
            multiwatch_poll(app);
            gui_redraw(app);
        }
        while (XPending(app->display)) {
            XEvent event; XNextEvent(app->display, &event);
            if (event.type == Expose) gui_redraw(app);
            else if (event.type == ConfigureNotify) { app->width = event.xconfigure.width; app->height = event.xconfigure.height; gui_redraw(app); }
            else if (event.type == ButtonPress) {
                if (event.xbutton.y < TAB_BAR_HEIGHT && event.xbutton.button == Button1) {
                    int x = event.xbutton.x;
                    int tab_index = (x - 6) / TAB_WIDTH;
                    int local_x = (x - 6) % TAB_WIDTH;
                    if (x >= 6 && tab_index >= 0 && tab_index < app->tab_count) {
                        if (local_x >= TAB_WIDTH - 25) close_tab(app, tab_index);
                        else app->active_tab = tab_index;
                    } else if (x >= app->tab_count * TAB_WIDTH + 7 && x < app->tab_count * TAB_WIDTH + PLUS_WIDTH + 7) create_tab(app);
                } else if (event.xbutton.button == Button4) app->tabs[app->active_tab].scroll_offset += 3;
                else if (event.xbutton.button == Button5) { app->tabs[app->active_tab].scroll_offset -= 3; if (app->tabs[app->active_tab].scroll_offset < 0) app->tabs[app->active_tab].scroll_offset = 0; }
                gui_redraw(app);
            } else if (event.type == KeyPress) {
                char text[32] = ""; KeySym key;
                int length = XLookupString(&event.xkey, text, sizeof(text) - 1, &key, NULL); text[length] = '\0';
                if (app->multiwatch.active && (event.xkey.state & ControlMask) &&
                    (key == XK_c || key == XK_C)) {
                    multiwatch_stop(app);
                } else {
                    hotkey_handle(app, &event.xkey, key, text, length);
                }
                gui_redraw(app);
            }
        }
    }
    if (app->multiwatch.active)
        multiwatch_stop(app);
}
