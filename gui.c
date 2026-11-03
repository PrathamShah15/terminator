#include <fcntl.h>
#include <sys/wait.h>
#include <pthread.h>
#include "terminator.h"
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

#define LEFT_MARGIN 14
#define TAB_BAR_HEIGHT 42
#define CONTENT_TOP 58
#define LINE_GAP 3
#define TAB_WIDTH 220
#define PLUS_WIDTH 48

typedef struct {char *text; int length;} DisplayLine;

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

//protect from memory leak
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
    XFillRectangle(app->display, app->backbuffer, app->gc, 0, 0, (unsigned)app->width, TAB_BAR_HEIGHT);
    XSetForeground(app->display, app->gc, app->border_color);
    XDrawLine(app->display, app->backbuffer, app->gc, 0, TAB_BAR_HEIGHT - 1, app->width, TAB_BAR_HEIGHT - 1);
    for (int i = 0; i < app->tab_count; i++) {
        int x = i * TAB_WIDTH + 6, width = TAB_WIDTH - 5;
        XSetForeground(app->display, app->gc, i == app->active_tab ? app->active_tab_background : app->tab_background);
        XFillRectangle(app->display, app->backbuffer, app->gc, x, 5, (unsigned)width, TAB_BAR_HEIGHT - 11);
        XSetForeground(app->display, app->gc, app->border_color);
        XDrawRectangle(app->display, app->backbuffer, app->gc, x, 5, (unsigned)width, TAB_BAR_HEIGHT - 11);
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
        XFillRectangle(app->display, app->backbuffer, app->gc, plus_x, 5, PLUS_WIDTH - 5, TAB_BAR_HEIGHT - 11);
        XSetForeground(app->display, app->gc, app->accent);
        XDrawRectangle(app->display, app->backbuffer, app->gc, plus_x, 5, PLUS_WIDTH - 5, TAB_BAR_HEIGHT - 11);
        int plus_width = text_width(app, "+", 1);
        int text_x = plus_x + (32 - plus_width) / 2 + 6;
        int text_y = 27;

        draw_text(app, text_x, text_y, "+", 1, &app->xft_accent);
    }
}

void tab_worker_loop(App *app, Tab *tab)
{
    if (tab->cwd[0] != '\0')
        chdir(tab->cwd);

    while (1) {
        jobs_poll(app);

        struct pollfd pfd;
        pfd.fd = tab->pipe_to_tab[0];
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 50);
        if (pr <= 0) continue;

        int len = 0;
        ssize_t n = read(tab->pipe_to_tab[0], &len, sizeof(len));
        if (n <= 0) break;
        if (len <= 0 || len >= INPUT_CAP) continue;

        char command[INPUT_CAP];
        int total = 0;
        while (total < len) {
            ssize_t r = read(tab->pipe_to_tab[0], command + total, len - total);
            if (r <= 0) break;
            total += (int)r;
        }
        if (total < len) break;
        command[len] = '\0';

        if (tab->shared) {
            tab->shared->busy = 1;
        }

        shell_execute(app, tab, command);

        if (tab->shared) {
            /* Preserve cwd set by builtin cd; do not overwrite */
            tab->shared->foreground_pgid = 0;
            tab->shared->busy = 0;
        }
    }
}

static void *tab_thread_worker(void *arg)
{
    Tab *tab = (Tab *)arg;
    App *app = tab->app_ref;
    tab_worker_loop(app, tab);
    return NULL;
}

void create_tab(App *app)
{
    if (app->tab_count >= MAX_TABS) return;

    int index = app->tab_count++;
    Tab *tab = &app->tabs[index];
    memset(tab, 0, sizeof(*tab));

    tab->alive = 1;
    tab->tab_id = index;
    tab->history_index = -1;
    tab->app_ref = app;
    if (app->tab_shared_pool) {
        tab->shared = &app->tab_shared_pool[index];
        memset(tab->shared, 0, sizeof(TabSharedState));
    }

    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL)
        snprintf(tab->cwd, sizeof(tab->cwd), "/");
    if (tab->shared)
        snprintf(tab->shared->cwd, sizeof(tab->shared->cwd), "%s", tab->cwd);

    if (pipe(tab->pipe_to_tab) < 0 || pipe(tab->pipe_from_tab) < 0) {
        perror("pipe failed");
        app->tab_count--;
        return;
    }

    if (app->use_process_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            close(tab->pipe_to_tab[0]); close(tab->pipe_to_tab[1]);
            close(tab->pipe_from_tab[0]); close(tab->pipe_from_tab[1]);
            app->tab_count--;
            return;
        }

        if (pid == 0) {
            close(tab->pipe_to_tab[1]);
            close(tab->pipe_from_tab[0]);
            tab->pipe_to_tab[1] = -1;
            tab->pipe_from_tab[0] = -1;

            if (app->display) {
                close(ConnectionNumber(app->display));
                app->display = NULL;
            }

            signal(SIGINT, SIG_IGN);
            signal(SIGTSTP, SIG_IGN);

            tab_worker_loop(app, tab);

            close(tab->pipe_to_tab[0]);
            close(tab->pipe_from_tab[1]);
            _exit(0);
        } else {
            close(tab->pipe_to_tab[0]);
            close(tab->pipe_from_tab[1]);
            tab->pipe_to_tab[0] = -1;
            tab->pipe_from_tab[1] = -1;
            tab->process_id = pid;

            int flags = fcntl(tab->pipe_from_tab[0], F_GETFL, 0);
            if (flags >= 0)
                fcntl(tab->pipe_from_tab[0], F_SETFL, flags | O_NONBLOCK);

            tab_append_raw(tab, "Tab created in Process Mode (useProcess=True).\n");
        }
    } else {
        int flags = fcntl(tab->pipe_from_tab[0], F_GETFL, 0);
        if (flags >= 0)
            fcntl(tab->pipe_from_tab[0], F_SETFL, flags | O_NONBLOCK);

        int err = pthread_create(&tab->thread_id, NULL, tab_thread_worker, tab);
        if (err != 0) {
            fprintf(stderr, "Failed to create tab thread: %d\n", err);
            close(tab->pipe_to_tab[0]); close(tab->pipe_to_tab[1]);
            close(tab->pipe_from_tab[0]); close(tab->pipe_from_tab[1]);
            app->tab_count--;
            return;
        }
        pthread_detach(tab->thread_id);
        tab_append_raw(tab, "Tab created in Thread Mode (Default, pthread_t active).\n");
    }

    app->active_tab = index;
}

void close_tab(App *app, int index)
{
    if (index < 0 || index >= app->tab_count) return;

    if (app->tab_count == 1) {
        app->running = 0;
        return;
    }

    Tab *tab = &app->tabs[index];

    if (app->use_process_mode) {
        if (tab->process_id > 0) {
            kill(tab->process_id, SIGTERM);
            waitpid(tab->process_id, NULL, WNOHANG);
        }
    } else {
        tab->alive = 0;
    }
    if (tab->pipe_to_tab[1] >= 0) {
        close(tab->pipe_to_tab[1]);
        tab->pipe_to_tab[1] = -1;
    }
    if (tab->pipe_from_tab[0] >= 0) {
        close(tab->pipe_from_tab[0]);
        tab->pipe_from_tab[0] = -1;
    }

    for (int i = index; i < app->tab_count - 1; i++) {
        app->tabs[i] = app->tabs[i + 1];
        if (app->tab_shared_pool) {
            app->tab_shared_pool[i] = app->tab_shared_pool[i + 1];
            app->tabs[i].shared = &app->tab_shared_pool[i];
        }
    }

    app->tab_count--;
    if (app->active_tab > index)
        app->active_tab--;
    else if (app->active_tab == index && app->active_tab >= app->tab_count)
        app->active_tab = app->tab_count - 1;

    if (app->active_tab < 0)
        app->active_tab = 0;
}


/* Convert a mouse position into a wrapped output-line/character position. */
static int selection_hit_test(App *app, DisplayLine *lines, size_t output_count,
                              size_t output_start, size_t output_end,
                              int x, int y, int *line_out, int *col_out)
{
    int first_y = CONTENT_TOP + font_height(app);
    if (y < first_y) return 0;
    int row = (y - first_y) / line_height(app);
    size_t visible_count = output_end - output_start;
    if (row < 0 || (size_t)row >= visible_count) return 0;

    size_t line_index = output_start + (size_t)row;
    if (line_index >= output_count) return 0;

    int target_x = x - LEFT_MARGIN;
    if (target_x < 0) target_x = 0;
    int col = 0;
    while (col < lines[line_index].length) {
        int width = text_width(app, lines[line_index].text, col + 1);
        if (width >= target_x) {
            if (col > 0) {
                int prev = text_width(app, lines[line_index].text, col);
                if (target_x - prev > width - target_x) col++;
            }
            break;
        }
        col++;
    }
    *line_out = (int)line_index;
    *col_out = col;
    return 1;
}

//allows to select in both directions
static void selection_order(const App *app, int *sl, int *sc, int *el, int *ec)
{
    *sl = app->selection_anchor_line;
    *sc = app->selection_anchor_col;
    *el = app->selection_end_line;
    *ec = app->selection_end_col;
    if (*sl > *el || (*sl == *el && *sc > *ec)) {
        int t = *sl; *sl = *el; *el = t;
        t = *sc; *sc = *ec; *ec = t;
    }
}

//extract selected text and place it in clipboard
void gui_copy_selection(App *app)
{
    if (!app || !app->selection_active || app->tab_count <= 0) return;

    Tab *tab = &app->tabs[app->active_tab];
    int width = app->width - LEFT_MARGIN * 2;
    if (width < 1) width = 1;
    DisplayLine *lines = NULL;
    size_t count = 0, capacity = 0;
    if (wrap_output(app, &lines, &count, &capacity, width) != 0) {
        free_lines(lines, count);
        return;
    }

    int sl, sc, el, ec;
    selection_order(app, &sl, &sc, &el, &ec);
    if (sl < 0 || el < sl || (size_t)el >= count) {
        free_lines(lines, count);
        return;
    }
    //bytes to allocate
    size_t total = 1;
    for (int i = sl; i <= el; i++) {
        int a = i == sl ? sc : 0;
        int b = i == el ? ec : lines[i].length;
        if (a < 0) a = 0;
        if (b > lines[i].length) b = lines[i].length;
        if (b > a) total += (size_t)(b - a);
        if (i != el) total++;
    }
    char *copy = malloc(total);
    if (!copy) { free_lines(lines, count); return; }
    size_t pos = 0;
    for (int i = sl; i <= el; i++) {
        int a = i == sl ? sc : 0;
        int b = i == el ? ec : lines[i].length;
        if (a < 0) a = 0;
        if (b > lines[i].length) b = lines[i].length;
        if (b > a) {
            memcpy(copy + pos, lines[i].text + a, (size_t)(b - a));
            pos += (size_t)(b - a);
        }
        if (i != el) copy[pos++] = '\n';
    }
    copy[pos] = '\0';

    free(app->clipboard_text);
    app->clipboard_text = copy;
    XSetSelectionOwner(app->display, app->clipboard_atom, app->window, CurrentTime);
    XSetSelectionOwner(app->display, XA_PRIMARY, app->window, CurrentTime);
    XFlush(app->display);
    free_lines(lines, count);
    (void)tab;
}

void gui_redraw(App *app)
{
    if (app->tab_count <= 0) return;
    if (app->active_tab < 0 || app->active_tab >= app->tab_count) app->active_tab = 0;
    Tab *tab = &app->tabs[app->active_tab];
    /*
     * Render into an off-screen pixmap. The window is updated only once the
     * complete frame is ready, preventing the clear-then-redraw flash.
     */
    if (app->backbuffer == None ||
        app->backbuffer_width != app->width ||
        app->backbuffer_height != app->height) {
        Pixmap old_backbuffer = app->backbuffer;
        app->backbuffer = XCreatePixmap(app->display, app->window,
                                        (unsigned)app->width,
                                        (unsigned)app->height,
                                        (unsigned)DefaultDepth(app->display,
                                            DefaultScreen(app->display)));
        if (app->backbuffer == None)
            return;
        app->backbuffer_width = app->width;
        app->backbuffer_height = app->height;
        if (app->xft_draw != NULL)
            XftDrawChange(app->xft_draw, app->backbuffer);
        if (old_backbuffer != None)
            XFreePixmap(app->display, old_backbuffer);
    }

    XSetForeground(app->display, app->gc, app->background);
    XFillRectangle(app->display, app->backbuffer, app->gc, 0, 0,
                   (unsigned)app->width, (unsigned)app->height);
    draw_tab_bar(app);
    int available_width = app->width - LEFT_MARGIN * 2;
    if (available_width < 1) available_width = 1;
    DisplayLine *lines = NULL; size_t count = 0, capacity = 0;
    if (wrap_output(app, &lines, &count, &capacity, available_width) != 0) { free_lines(lines, count); XFlush(app->display); return; }
    char prompt[PATH_MAX + 64];
    const char *visible_input = tab->input;
    if (app->history_search_mode) {
        snprintf(prompt, sizeof(prompt), "Enter search term: ");
        visible_input = app->history_search;
    } else if (tab->continuing) {
        snprintf(prompt, sizeof(prompt), "> ");
    } else {
        const char *cwd = (tab->shared && tab->shared->cwd[0]) ? tab->shared->cwd : tab->cwd;
        snprintf(prompt, sizeof(prompt), "pratham@terminator:%s$ ", cwd);
    }
    char input_line[PATH_MAX + INPUT_CAP + 128];
    snprintf(input_line, sizeof(input_line), "%s%s", prompt, visible_input);
    size_t prompt_length = strlen(prompt), cursor_position = prompt_length + (app->history_search_mode ? strlen(app->history_search) : tab->cursor);
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
    
    int sel_sl = 0, sel_sc = 0, sel_el = 0, sel_ec = 0;
    if (app->selection_active)
        selection_order(app, &sel_sl, &sel_sc, &sel_el, &sel_ec);
    for (size_t i = output_start; i < output_end; i++) {
        int selected = app->selection_active &&
                       (int)i >= sel_sl && (int)i <= sel_el;
        if (selected) {
            int a = (int)i == sel_sl ? sel_sc : 0;
            int b = (int)i == sel_el ? sel_ec : lines[i].length;
            if (a < 0) a = 0;
            if (a > lines[i].length) a = lines[i].length;
            if (b > lines[i].length) b = lines[i].length;
            if (b > a) {
                int sx = LEFT_MARGIN + text_width(app, lines[i].text, a);
                int sw = text_width(app, lines[i].text + a, b - a);
                XSetForeground(app->display, app->gc, app->active_tab_background);
                XFillRectangle(app->display, app->backbuffer, app->gc,
                               sx, y - font_height(app) + 3,
                               (unsigned)(sw > 1 ? sw : 2),
                               (unsigned)line_height(app));
                draw_text(app, LEFT_MARGIN, y, lines[i].text, a,
                          &app->xft_foreground);//before selection
                draw_text(app, sx, y, lines[i].text + a, b - a,
                          &app->xft_foreground); //in highlighted area
                draw_text(app, sx + sw, y, lines[i].text + b,
                          lines[i].length - b, &app->xft_foreground); //after selection
            } else {
                draw_text(app, LEFT_MARGIN, y, lines[i].text,
                          lines[i].length, &app->xft_foreground);
            }
        } else {
            draw_text(app, LEFT_MARGIN, y, lines[i].text,
                      lines[i].length, &app->xft_foreground);
        }
        y += line_height(app);
    }

    // if (tab->scroll_offset == 0 && output_rows > 0 && output_count > output_rows)
    //     y = first_y + (int)(output_rows - (output_end - output_start)) * line_height(app);
    for (size_t i = input_start_index; i < count; i++) {
        const char *line = lines[i].text; 
        size_t line_length = (size_t)lines[i].length, global_start = 0;
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
            XDrawLine(app->display, app->backbuffer, app->gc, cursor_x, y - font_height(app) + 4, cursor_x, y + 4);
        }
        y += line_height(app);
    }
    /* Render filename candidates below the input line, with the active item highlighted. */
    if (app->completion_active && app->completion_count > 0) {
        int visible = app->completion_count;
        int max_visible = (app->height - y - 8) / line_height(app);
        if (max_visible < 1) max_visible = 1;
        if (visible > max_visible) visible = max_visible;
        for (int i = 0; i < visible; i++) {
            int row_y = y + i * line_height(app);
            if (i == app->completion_selected) {
                XSetForeground(app->display, app->gc, app->active_tab_background);
                XFillRectangle(app->display, app->backbuffer, app->gc,
                               LEFT_MARGIN - 4, row_y - font_height(app) + 3,
                               (unsigned)(app->width - LEFT_MARGIN * 2),
                               (unsigned)line_height(app));
            }
            char candidate[PATH_MAX + 32];
            snprintf(candidate, sizeof(candidate), "%s%s",
                     i == app->completion_selected ? "> " : "  ",
                     app->completion_matches[i]);
            draw_text(app, LEFT_MARGIN, row_y, candidate, (int)strlen(candidate),
                      i == app->completion_selected ? &app->xft_accent : &app->xft_foreground);
        }
    }
    free_lines(lines, count);
    XCopyArea(app->display, app->backbuffer, app->window, app->gc,
              0, 0, (unsigned)app->width, (unsigned)app->height, 0, 0);
    XFlush(app->display);
}


static void handle_selection_request(App *app, XSelectionRequestEvent *request)
{
    XSelectionEvent response;
    memset(&response, 0, sizeof(response));
    response.type = SelectionNotify;
    response.display = request->display;
    response.requestor = request->requestor;
    response.selection = request->selection;
    response.target = request->target;
    response.time = request->time;
    response.property = request->property == None
                      ? request->target : request->property;

    if (request->target == app->targets_atom) {
        Atom targets[] = { app->targets_atom, app->utf8_string_atom, XA_STRING };
        XChangeProperty(app->display, request->requestor, request->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)targets, 3);
    } else if (request->target == app->utf8_string_atom ||
               request->target == XA_STRING) {
        if (app->clipboard_text == NULL) {
            response.property = None;
        } else {
            XChangeProperty(app->display, request->requestor,
                            request->property, request->target, 8,
                            PropModeReplace,
                            (unsigned char *)app->clipboard_text,
                            (int)strlen(app->clipboard_text));
        }
    } else {
        response.property = None;
    }

    XSendEvent(app->display, request->requestor, False, 0,
               (XEvent *)&response);
    XFlush(app->display);
}

static void handle_selection_notify(App *app, XSelectionEvent *event)
{
    if (event->property == None)
        return;

    Atom actual_type;
    int actual_format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(app->display, app->window, event->property,
                           0, INPUT_CAP, False, AnyPropertyType,
                           &actual_type, &actual_format, &count,
                           &remaining, &data) == Success &&
        data != NULL && actual_format == 8 &&
        (actual_type == app->utf8_string_atom || actual_type == XA_STRING)) {
        Tab *tab = &app->tabs[app->active_tab];
        size_t old_length = strlen(tab->input);
        size_t insert_length = (size_t)count;
        if (insert_length > sizeof(tab->input) - old_length - 1)
            insert_length = sizeof(tab->input) - old_length - 1;
        memmove(tab->input + tab->cursor + insert_length,
                tab->input + tab->cursor,
                old_length - tab->cursor + 1);
        memcpy(tab->input + tab->cursor, data, insert_length);
        tab->cursor += insert_length;
        XFree(data);
    } else if (data != NULL) {
        XFree(data);
    }
    XDeleteProperty(app->display, app->window, event->property);
}

static int background_activity_exists(const App *app)
{
    if (app->multiwatch.active)
        return 1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (app->jobs[i].active)
            return 1;
    }
    return 0;
}

void gui_run(App *app)
{
    int x_fd = ConnectionNumber(app->display);

    /* Initial frame. Thereafter, idle windows are not continuously redrawn. */
    gui_redraw(app);

    while (app->running) {
        jobs_poll(app);
        multiwatch_poll(app);

        struct pollfd fds[1 + MAX_TABS];
        int tab_indices[1 + MAX_TABS];
        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        int nfds = 1;

        for (int i = 0; i < app->tab_count; i++) {
            Tab *t = &app->tabs[i];
            if (t->alive && t->pipe_from_tab[0] >= 0) {
                fds[nfds].fd = t->pipe_from_tab[0];
                fds[nfds].events = POLLIN | POLLHUP;
                tab_indices[nfds] = i;
                nfds++;
            }
        }

        int timeout = (background_activity_exists(app) || app->multiwatch.active) ? 50 : 50;
        if (!XPending(app->display)) {
            (void)poll(fds, nfds, timeout);
        }

        int needs_redraw = 0;
        for (int k = 1; k < nfds; k++) {
            if (fds[k].revents & (POLLIN | POLLHUP)) {
                int t_idx = tab_indices[k];
                Tab *t = &app->tabs[t_idx];
                char buffer[4096];
                ssize_t bytes = read(t->pipe_from_tab[0], buffer, sizeof(buffer) - 1);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    tab_append_raw(t, buffer);
                    needs_redraw = 1;
                } else if (bytes == 0) {
                    close_tab(app, t_idx);
                    needs_redraw = 1;
                    break;
                }
            }
        }

        if (background_activity_exists(app)) {
            jobs_poll(app);
            multiwatch_poll(app);
            needs_redraw = 1;
        }

        if (needs_redraw)
            gui_redraw(app);

        while (XPending(app->display)) {
            XEvent event;
            XNextEvent(app->display, &event);

            if (event.type == SelectionRequest) {
                handle_selection_request(app, &event.xselectionrequest);
            } else if (event.type == SelectionNotify) {
                handle_selection_notify(app, &event.xselection);
                gui_redraw(app);
            } else if (event.type == Expose) {
                if (event.xexpose.count == 0)
                    gui_redraw(app);
            } else if (event.type == ConfigureNotify) {
                app->width = event.xconfigure.width;
                app->height = event.xconfigure.height;
                gui_redraw(app);
            } else if (event.type == ButtonPress) {
                if (event.xbutton.button == Button1 &&
                    event.xbutton.y < TAB_BAR_HEIGHT) {
                    int x = event.xbutton.x;
                    int tab_index = (x - 6) / TAB_WIDTH;
                    int local_x = (x - 6) % TAB_WIDTH;

                    app->selection_active = 0;
                    if (x >= 6 && tab_index >= 0 &&
                        tab_index < app->tab_count) {
                        if (local_x >= 185 && local_x <= 200)
                            close_tab(app, tab_index);
                        else
                            app->active_tab = tab_index;
                    } else if (x >= app->tab_count * TAB_WIDTH + 7 &&
                               x < app->tab_count * TAB_WIDTH + PLUS_WIDTH + 7) {
                        create_tab(app);
                    }
                } else if (event.xbutton.button == Button1) {
                    Tab *active = &app->tabs[app->active_tab];
                    int width = app->width - LEFT_MARGIN * 2;
                    if (width < 1) width = 1;
                    DisplayLine *lines = NULL;
                    size_t count = 0, capacity = 0;
                    if (wrap_output(app, &lines, &count, &capacity, width) == 0) {
                        char prompt[PATH_MAX + 64];
                        const char *visible_input = active->input;
                        if (app->history_search_mode) {
                            snprintf(prompt, sizeof(prompt), "Enter search term: ");
                            visible_input = app->history_search;
                        } else if (active->continuing) {
                            snprintf(prompt, sizeof(prompt), "> ");
                        } else {
                            const char *cwd = (active->shared && active->shared->cwd[0]) ? active->shared->cwd : active->cwd;
                            snprintf(prompt, sizeof(prompt), "pratham@terminator:%s$ ", cwd);
                        }
                        char input_line[PATH_MAX + INPUT_CAP + 128];
                        snprintf(input_line, sizeof(input_line), "%s%s", prompt, visible_input);
                        size_t output_count = count, cap = capacity;
                        if (wrap_text(app, &lines, &count, &cap, input_line,
                                      strlen(input_line), width) == 0) {
                            int first_y = CONTENT_TOP + font_height(app);
                            int rows = (app->height - first_y - 8) / line_height(app);
                            if (rows < 1) rows = 1;
                            size_t input_rows = count - output_count;
                            if (input_rows > (size_t)rows) input_rows = (size_t)rows;
                            size_t output_rows = (size_t)rows - input_rows;
                            size_t scroll = active->scroll_offset < 0 ? 0 : (size_t)active->scroll_offset;
                            if (scroll > output_count) scroll = output_count;
                            size_t output_end = output_count - scroll;
                            size_t output_start = output_end > output_rows ?
                                output_end - output_rows : 0;
                            int line, col;
                            if (selection_hit_test(app, lines, output_count,
                                                   output_start, output_end,
                                                   event.xbutton.x, event.xbutton.y,
                                                   &line, &col)) {
                                app->selecting = 1;
                                app->selection_active = 1;
                                app->selection_anchor_line = line;
                                app->selection_anchor_col = col;
                                app->selection_end_line = line;
                                app->selection_end_col = col;
                            } else {
                                app->selecting = 0;
                                app->selection_active = 0;
                            }
                        }
                        free_lines(lines, count);
                    }
                } else if (event.xbutton.button == Button4) {
                    app->tabs[app->active_tab].scroll_offset += 3;
                    app->selection_active = 0;
                } else if (event.xbutton.button == Button5) {
                    app->tabs[app->active_tab].scroll_offset -= 3;
                    if (app->tabs[app->active_tab].scroll_offset < 0)
                        app->tabs[app->active_tab].scroll_offset = 0;
                    app->selection_active = 0;
                }
                gui_redraw(app);
            } else if (event.type == MotionNotify && app->selecting) {
                Tab *active = &app->tabs[app->active_tab];
                int width = app->width - LEFT_MARGIN * 2;
                if (width < 1) width = 1;
                DisplayLine *lines = NULL;
                size_t count = 0, capacity = 0;
                if (wrap_output(app, &lines, &count, &capacity, width) == 0) {
                    char prompt[PATH_MAX + 64];
                    const char *visible_input = active->input;
                    if (app->history_search_mode) {
                        snprintf(prompt, sizeof(prompt), "Enter search term: ");
                        visible_input = app->history_search;
                    } else if (active->continuing) {
                        snprintf(prompt, sizeof(prompt), "> ");
                    } else {
                        const char *cwd = (active->shared && active->shared->cwd[0]) ? active->shared->cwd : active->cwd;
                        snprintf(prompt, sizeof(prompt), "pratham@terminator:%s$ ", cwd);
                    }
                    char input_line[PATH_MAX + INPUT_CAP + 128];
                    
                    snprintf(input_line, sizeof(input_line), "%s%s", prompt, visible_input);
                    size_t output_count = count, cap = capacity;
                    if (wrap_text(app, &lines, &count, &cap, input_line,
                                  strlen(input_line), width) == 0) {
                        int first_y = CONTENT_TOP + font_height(app);
                        int rows = (app->height - first_y - 8) / line_height(app);
                        if (rows < 1) rows = 1;
                        size_t input_rows = count - output_count;
                        if (input_rows > (size_t)rows) input_rows = (size_t)rows;
                        size_t output_rows = (size_t)rows - input_rows;
                        size_t scroll = active->scroll_offset < 0 ? 0 : (size_t)active->scroll_offset;
                        if (scroll > output_count) scroll = output_count;
                        size_t output_end = output_count - scroll;
                        size_t output_start = output_end > output_rows ?
                            output_end - output_rows : 0;
                        int line, col;
                        if (selection_hit_test(app, lines, output_count,
                                               output_start, output_end,
                                               event.xmotion.x, event.xmotion.y,
                                               &line, &col)) {
                            app->selection_end_line = line;
                            app->selection_end_col = col;
                        }
                    }
                    free_lines(lines, count);
                }
                gui_redraw(app);
            } else if (event.type == ButtonRelease &&
                       event.xbutton.button == Button1) {
                app->selecting = 0;
                gui_redraw(app);
            } else if (event.type == KeyPress) {
                char text[32] = "";
                KeySym key;
                int length = XLookupString(&event.xkey, text,
                                           sizeof(text) - 1, &key, NULL);
                if (length < 0)
                    length = 0;
                text[length] = '\0';

                if (app->multiwatch.active &&
                    (event.xkey.state & ControlMask) &&
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
