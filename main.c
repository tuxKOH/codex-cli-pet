#define _GNU_SOURCE

#include "config.h"
#include "settings.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PET_SIZE 184
#define POLL_MS 16
#define CODEX_SCAN_MS 250
#define CLICK_SLOP 4
#define ANIMATION_MS 300

typedef struct {
    Window id;
    pid_t pid;
    int x;
    int y;
    unsigned width;
    unsigned height;
    char title[256];
    char wm_class[256];
} Terminal;

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window pet;
    Visual *visual;
    Colormap colormap;
    cairo_surface_t *x_surface;
    cairo_surface_t *back_buffer;
    cairo_surface_t *pet_image;
    cairo_surface_t *bubble_image;
    Atom client_list;
    Atom active_window;
    Atom wm_pid;
    Atom wm_name;
    Atom utf8_string;
    Atom net_wm_window_type;
    Atom net_wm_window_type_utility;
    Atom net_wm_state;
    Atom net_wm_state_above;
    Atom net_wm_state_skip_taskbar;
    Atom net_wm_state_skip_pager;
    Terminal terminal;
    int has_terminal;
    int relative_x;
    int relative_y;
    int screen_x;
    int screen_y;
    int visible;
    int dragging;
    int press_root_x;
    int press_root_y;
    int press_window_x;
    int press_window_y;
    double animation_scale;
    long long animation_started;
    int demo;
    pid_t *codex_pids;
    size_t codex_count;
    long long codex_scan_at;
    PetConfig config;
    char config_path[PATH_MAX];
    char directory[PATH_MAX];
    char bubble_text[PET_CONFIG_VALUE_MAX];
    long long config_check_at;
    time_t config_mtime;
    pid_t fetch_pid;
    int fetch_index;
    char fetch_path[PATH_MAX];
    char sound_path[PATH_MAX];
    char audio_device[PET_CONFIG_AUDIO_DEVICE_MAX];
    pid_t sound_pid;
    long long press_started;
    int bubble_pressed;
    int long_press_sent;
} App;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void resolve_sound_path(App *app, const char *directory);

static void die(const char *message) {
    fprintf(stderr, "codex-pet: %s\n", message);
    exit(1);
}

static int x_error_handler(Display *display, XErrorEvent *event) {
    char message[256];
    (void)display;
    /* The EWMH client list is inherently racy: a window may disappear
     * between reading the list and querying it. Xlib's default handler exits
     * the whole overlay for that harmless case. */
    if (event->error_code != BadWindow && event->error_code != BadDrawable) {
        XGetErrorText(display, event->error_code, message, sizeof(message));
        fprintf(stderr, "codex-pet: X11 warning: %s (request %d)\n",
                message, event->request_code);
    }
    return 0;
}

static int read_file(const char *path, char *buffer, size_t capacity) {
    FILE *file = fopen(path, "rb");
    size_t length;
    if (!file) return 0;
    length = fread(buffer, 1, capacity - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return (int)length;
}

static int parse_pid_name(const char *name, pid_t *pid) {
    char *end;
    long value;
    if (!name || !*name) return 0;
    errno = 0;
    value = strtol(name, &end, 10);
    if (errno || *end || value <= 0 || value > INT_MAX) return 0;
    *pid = (pid_t)value;
    return 1;
}

static int process_parent(pid_t pid, pid_t *parent) {
    char path[64];
    char stat_line[4096];
    char *close_paren;
    char state;
    unsigned long ppid;
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (!read_file(path, stat_line, sizeof(stat_line))) return 0;
    close_paren = strrchr(stat_line, ')');
    if (!close_paren || sscanf(close_paren + 2, "%c %lu", &state, &ppid) != 2) {
        return 0;
    }
    *parent = (pid_t)ppid;
    return 1;
}

static int contains_token(const char *command, const char *wanted) {
    const char *cursor = command;
    size_t wanted_length = strlen(wanted);
    while (*cursor) {
        const char *start;
        while (*cursor && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')) cursor++;
        start = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\n') cursor++;
        if ((size_t)(cursor - start) == wanted_length &&
            strncmp(start, wanted, wanted_length) == 0) return 1;
    }
    return 0;
}

static int is_codex_process(pid_t pid) {
    char path[64];
    char comm[256];
    char command[4096];
    char normalized[4096];
    size_t i, command_length;
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    if (!read_file(path, comm, sizeof(comm))) return 0;
    comm[strcspn(comm, "\r\n")] = '\0';
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    command_length = (size_t)read_file(path, command, sizeof(command));
    for (i = 0; i < command_length; i++) {
        if (command[i] == '\0') command[i] = ' ';
        if (command[i] == '/' || command[i] == '@') command[i] = ' ';
    }
    strncpy(normalized, command, sizeof(normalized));
    normalized[sizeof(normalized) - 1] = '\0';
    if (strstr(normalized, "codex-pet")) return 0;
    if (!strcmp(comm, "codex") || !strcmp(comm, "codex-cli") || !strcmp(comm, "codex.exe")) return 1;
    return contains_token(normalized, "codex") ||
           contains_token(normalized, "codex-cli") ||
           contains_token(normalized, "codex.exe");
}

static int is_ancestor(pid_t ancestor, pid_t child) {
    pid_t current = child;
    int steps;
    for (steps = 0; current > 1 && steps < 64; steps++) {
        pid_t parent;
        if (current == ancestor) return 1;
        if (!process_parent(current, &parent) || parent == current) break;
        current = parent;
    }
    return current == ancestor;
}

static void refresh_codex_cache(App *app) {
    DIR *directory;
    struct dirent *entry;
    size_t count = 0;
    pid_t *pids = NULL;
    if (app->demo) {
        app->codex_scan_at = now_ms();
        return;
    }
    directory = opendir("/proc");
    if (!directory) return;
    while ((entry = readdir(directory))) {
        pid_t pid;
        if (!parse_pid_name(entry->d_name, &pid)) continue;
        if (is_codex_process(pid)) {
            pid_t *grown = realloc(pids, (count + 1) * sizeof(*pids));
            if (!grown) break;
            pids = grown;
            pids[count++] = pid;
        }
    }
    closedir(directory);
    free(app->codex_pids);
    app->codex_pids = pids;
    app->codex_count = count;
    app->codex_scan_at = now_ms();
}

static int has_codex_ancestor(App *app, pid_t terminal_pid) {
    size_t i;
    if (app->demo) return 1;
    if (now_ms() - app->codex_scan_at >= CODEX_SCAN_MS) refresh_codex_cache(app);
    for (i = 0; i < app->codex_count; i++) {
        if (is_ancestor(terminal_pid, app->codex_pids[i])) return 1;
    }
    return 0;
}

static int read_atom_property(Display *display, Window window, Atom property,
                              Atom requested_type, unsigned long *value) {
    Atom actual_type;
    int actual_format;
    unsigned long item_count, bytes_after;
    unsigned char *data = NULL;
    int status = XGetWindowProperty(display, window, property, 0, 1, False,
                                    requested_type, &actual_type, &actual_format,
                                    &item_count, &bytes_after, &data);
    if (status != Success || !data || item_count < 1) {
        if (data) XFree(data);
        return 0;
    }
    if (actual_format == 32) *value = ((unsigned long *)data)[0];
    else if (actual_format == 16) *value = ((unsigned short *)data)[0];
    else *value = data[0];
    XFree(data);
    return 1;
}

static void read_text_property(Display *display, Window window, Atom property,
                               Atom type, char *output, size_t capacity) {
    Atom actual_type;
    int actual_format;
    unsigned long item_count, bytes_after;
    unsigned char *data = NULL;
    output[0] = '\0';
    if (XGetWindowProperty(display, window, property, 0, 256, False, type,
                           &actual_type, &actual_format, &item_count,
                           &bytes_after, &data) != Success || !data) return;
    if (actual_format == 8) {
        size_t copy_length = item_count < capacity - 1 ? item_count : capacity - 1;
        memcpy(output, data, copy_length);
        output[copy_length] = '\0';
    }
    XFree(data);
}

static int looks_like_terminal(const Terminal *terminal) {
    static const char *words[] = {
        "alacritty", "kitty", "gnome-terminal", "gnome terminal", "konsole",
        "xterm", "wezterm", "xfce4-terminal", "foot", "tilix", "terminator",
        "qterminal", "st-", "mate-terminal", "urxvt", "terminology", "tabby", "hyper"
    };
    char text[600];
    size_t i;
    snprintf(text, sizeof(text), "%s %s", terminal->wm_class, terminal->title);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcasestr(text, words[i])) return 1;
    }
    return 0;
}

static int get_active_window(App *app, Window *window) {
    unsigned long value;
    if (!read_atom_property(app->display, app->root, app->active_window, XA_WINDOW, &value)) return 0;
    *window = (Window)value;
    return 1;
}

static int get_terminal_info(App *app, Window window, Terminal *terminal) {
    XWindowAttributes attributes;
    Window child;
    unsigned long pid_value;
    XClassHint class_hint;
    int root_x, root_y;
    if (!XGetWindowAttributes(app->display, window, &attributes)) return 0;
    if (!read_atom_property(app->display, window, app->wm_pid, XA_CARDINAL, &pid_value)) return 0;
    if (!XTranslateCoordinates(app->display, window, app->root, 0, 0,
                               &root_x, &root_y, &child)) return 0;
    memset(terminal, 0, sizeof(*terminal));
    terminal->id = window;
    terminal->pid = (pid_t)pid_value;
    terminal->x = root_x;
    terminal->y = root_y;
    terminal->width = (unsigned)attributes.width;
    terminal->height = (unsigned)attributes.height;
    read_text_property(app->display, window, app->wm_name, app->utf8_string,
                       terminal->title, sizeof(terminal->title));
    if (XGetClassHint(app->display, window, &class_hint)) {
        snprintf(terminal->wm_class, sizeof(terminal->wm_class), "%s %s",
                 class_hint.res_name ? class_hint.res_name : "",
                 class_hint.res_class ? class_hint.res_class : "");
        if (class_hint.res_name) XFree(class_hint.res_name);
        if (class_hint.res_class) XFree(class_hint.res_class);
    }
    return looks_like_terminal(terminal);
}

static int find_target(App *app, Terminal *target) {
    unsigned long item_count, bytes_after;
    Atom actual_type;
    int actual_format;
    unsigned char *data = NULL;
    Window active;
    unsigned long i;
    int found = 0;
    if (!get_active_window(app, &active)) return 0;
    if (XGetWindowProperty(app->display, app->root, app->client_list, 0, 4096,
                           False, XA_WINDOW, &actual_type, &actual_format,
                           &item_count, &bytes_after, &data) != Success || !data) return 0;
    for (i = 0; i < item_count; i++) {
        Terminal candidate;
        Window id = ((Window *)data)[i];
        if (id != active || !get_terminal_info(app, id, &candidate)) continue;
        if (has_codex_ancestor(app, candidate.pid)) {
            *target = candidate;
            found = 1;
        }
        break;
    }
    XFree(data);
    return found;
}

static long long clamp_position(App *app, int x, int y, int *out_x, int *out_y) {
    Terminal *terminal = &app->terminal;
    if (terminal->width < PET_SIZE || terminal->height < PET_SIZE) return 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > (int)terminal->width - PET_SIZE) x = (int)terminal->width - PET_SIZE;
    if (y > (int)terminal->height - PET_SIZE) y = (int)terminal->height - PET_SIZE;
    *out_x = x;
    *out_y = y;
    return 1;
}

static void render(App *app) {
    cairo_t *context;
    cairo_t *present;
    double scale = app->animation_scale;
    if (!app->visible || !app->x_surface || !app->back_buffer) return;
    context = cairo_create(app->back_buffer);
    cairo_set_operator(context, CAIRO_OPERATOR_CLEAR);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);

    // The chat bubble is intentionally outside the animation transform:
    // its size and top-left position never change when the pet is clicked.
    cairo_save(context);
    cairo_scale(context, 112.0 / cairo_image_surface_get_width(app->bubble_image),
                98.0 / cairo_image_surface_get_height(app->bubble_image));
    cairo_set_source_surface(context, app->bubble_image, 0, 0);
    cairo_paint(context);
    cairo_restore(context);

    if (app->bubble_text[0]) {
        PangoLayout *layout = pango_cairo_create_layout(context);
        PangoFontDescription *font = pango_font_description_from_string("Sans 10");
        pango_layout_set_text(layout, app->bubble_text, -1);
        pango_layout_set_width(layout, 88 * PANGO_SCALE);
        pango_layout_set_height(layout, 58 * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_font_description(layout, font);
        cairo_save(context);
        cairo_set_source_rgb(context, 0.22, 0.12, 0.40);
        cairo_translate(context, 12, 20);
        pango_cairo_show_layout(context, layout);
        cairo_restore(context);
        pango_font_description_free(font);
        g_object_unref(layout);
    }

    cairo_save(context);
    // Shrink only the character around its own center (113,119).
    cairo_translate(context, 113, 119);
    cairo_scale(context, scale, scale);
    cairo_translate(context, 42 - 113, 48 - 119);
    cairo_scale(context, 142.0 / cairo_image_surface_get_width(app->pet_image),
                142.0 / cairo_image_surface_get_height(app->pet_image));
    cairo_set_source_surface(context, app->pet_image, 0, 0);
    cairo_paint(context);
    cairo_restore(context);
    cairo_destroy(context);
    cairo_surface_flush(app->back_buffer);
    present = cairo_create(app->x_surface);
    cairo_set_operator(present, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(present, app->back_buffer, 0, 0);
    cairo_paint(present);
    cairo_destroy(present);
    cairo_surface_flush(app->x_surface);
    XFlush(app->display);
}

static void config_update_mtime(App *app);

static void set_bubble_text(App *app, const char *text) {
    snprintf(app->bubble_text, sizeof(app->bubble_text), "%s", text ? text : "");
    render(app);
}

static void show_active_value(App *app) {
    PetConfigItem *item;
    if (app->config.count <= 0) {
        set_bubble_text(app, "暂无显示配置");
        return;
    }
    if (app->config.active < 0 || app->config.active >= app->config.count)
        app->config.active = 0;
    item = &app->config.items[app->config.active];
    set_bubble_text(app, item->value[0] ? item->value : "暂无数据");
}

static void switch_bubble_config(App *app) {
    if (app->config.count <= 0) {
        set_bubble_text(app, "暂无显示配置");
        return;
    }
    app->config.active = (app->config.active + 1) % app->config.count;
    if (!pet_config_save(&app->config)) {
        set_bubble_text(app, "保存失败");
        return;
    }
    config_update_mtime(app);
    show_active_value(app);
}

static void open_settings(App *app) {
    pid_t child = fork();
    if (child == 0) {
        /* Make the editor a completely separate process. Closing the editor
         * must never run the overlay's event loop or teardown path. */
        setsid();
        close(ConnectionNumber(app->display));
        execl("/proc/self/exe", "codex-pet", "--settings", app->config_path,
              (char *)NULL);
        _exit(127);
    }
}

static void config_update_mtime(App *app) {
    struct stat info;
    if (!stat(app->config_path, &info)) app->config_mtime = info.st_mtime;
}

static void reload_config_if_changed(App *app) {
    struct stat info;
    long long now = now_ms();
    if (now - app->config_check_at < 500) return;
    app->config_check_at = now;
    if (stat(app->config_path, &info)) return;
    if (info.st_mtime != app->config_mtime) {
        int old_active = app->config.active;
        pet_config_load(&app->config, app->config_path);
        snprintf(app->audio_device, sizeof(app->audio_device), "%s",
                 app->config.audio_device);
        resolve_sound_path(app, app->directory);
        config_update_mtime(app);
        if (app->config.count > 0 && app->config.active != old_active) {
            set_bubble_text(app, app->config.items[app->config.active].value);
        }
    }
}

static void start_fetch(App *app) {
    PetConfigItem *item;
    pid_t child;
    if (app->fetch_pid > 0 || app->config.count <= 0) return;
    if (app->config.active < 0 || app->config.active >= app->config.count) app->config.active = 0;
    item = &app->config.items[app->config.active];
    if (!item->curl[0]) {
        set_bubble_text(app, "未配置 curl");
        return;
    }
    snprintf(app->fetch_path, sizeof(app->fetch_path), "/tmp/codex-pet-fetch-%d.json", (int)getpid());
    child = fork();
    if (child == 0) {
        FILE *output = fopen(app->fetch_path, "w");
        char raw[65536];
        int ok = output && pet_config_fetch_text(item, raw, sizeof(raw));
        if (output) {
            if (ok) fputs(raw, output);
            fclose(output);
        }
        _exit(ok ? 0 : 1);
    }
    if (child < 0) {
        set_bubble_text(app, "刷新失败");
        return;
    }
    app->fetch_pid = child;
    app->fetch_index = app->config.active;
    set_bubble_text(app, "刷新中...");
}

static void complete_fetch(App *app) {
    int status;
    char raw[65536];
    if (app->fetch_pid <= 0) return;
    if (waitpid(app->fetch_pid, &status, WNOHANG) != app->fetch_pid) return;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        read_file(app->fetch_path, raw, sizeof(raw)) > 0 &&
        app->fetch_index >= 0 && app->fetch_index < app->config.count) {
        PetConfigItem *item = &app->config.items[app->fetch_index];
        char formatted[PET_CONFIG_VALUE_MAX];
        if (pet_config_format_value(item, raw, formatted, sizeof(formatted))) {
            snprintf(item->value, sizeof(item->value), "%s", formatted);
            pet_config_save(&app->config);
            config_update_mtime(app);
            set_bubble_text(app, formatted);
        } else set_bubble_text(app, "解析失败");
    } else set_bubble_text(app, "刷新失败");
    unlink(app->fetch_path);
    app->fetch_pid = 0;
}

static void complete_sound(App *app) {
    int status;
    if (app->sound_pid <= 0) return;
    if (waitpid(app->sound_pid, &status, WNOHANG) == app->sound_pid)
        app->sound_pid = 0;
}

static void play_sound(App *app) {
    pid_t child;
    if (!app->sound_path[0] || access(app->sound_path, R_OK) != 0) return;
    complete_sound(app);
    /* Do not stack many copies when the button is clicked repeatedly while
     * the effect is still playing. The UI thread never waits for audio. */
    if (app->sound_pid > 0) return;
    child = fork();
    if (child == 0) {
        if (app->audio_device[0]) {
            int audio_pipe[2];
            pid_t decoder;
            int decoder_status;
            if (pipe(audio_pipe) == 0) {
                decoder = fork();
                if (decoder == 0) {
                    close(audio_pipe[0]);
                    dup2(audio_pipe[1], STDOUT_FILENO);
                    close(audio_pipe[1]);
                    execlp("ffmpeg", "ffmpeg", "-v", "error", "-i",
                           app->sound_path, "-f", "wav", "-", (char *)NULL);
                    _exit(127);
                }
                if (decoder > 0) {
                    close(audio_pipe[1]);
                    dup2(audio_pipe[0], STDIN_FILENO);
                    close(audio_pipe[0]);
                    execlp("paplay", "paplay", "--device", app->audio_device,
                           "--file-format=wav", "-", (char *)NULL);
                    waitpid(decoder, &decoder_status, 0);
                }
            }
            _exit(127);
        }
        /* paplay generally cannot decode MP3 files. ffplay handles the
         * bundled MP3 reliably; the other players remain fallbacks for a
         * user-configured audio file. */
        execlp("ffplay", "ffplay", "-nodisp", "-vn", "-autoexit",
               "-nostdin", "-loglevel", "quiet", app->sound_path,
               (char *)NULL);
        execlp("paplay", "paplay", app->sound_path, (char *)NULL);
        execlp("mpg123", "mpg123", "-q", app->sound_path, (char *)NULL);
        _exit(127);
    }
    if (child > 0) app->sound_pid = child;
}

static void resolve_sound_path(App *app, const char *directory) {
    char resolved[PATH_MAX];
    const char *home;
    if (!app->config.sound_path[0]) {
        app->sound_path[0] = '\0';
        return;
    }
    if (app->config.sound_path[0] == '/') {
        snprintf(app->sound_path, sizeof(app->sound_path), "%s",
                 app->config.sound_path);
    } else if (app->config.sound_path[0] == '~' &&
               app->config.sound_path[1] == '/') {
        home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *password = getpwuid(getuid());
            home = password ? password->pw_dir : ".";
        }
        snprintf(resolved, sizeof(resolved), "%s/%s", home,
                 app->config.sound_path + 2);
        snprintf(app->sound_path, sizeof(app->sound_path), "%s", resolved);
    } else {
        size_t directory_length = strlen(directory);
        size_t sound_length = strlen(app->config.sound_path);
        if (directory_length + 1 + sound_length >= sizeof(resolved)) {
            app->sound_path[0] = '\0';
            return;
        }
        memcpy(resolved, directory, directory_length);
        resolved[directory_length] = '/';
        memcpy(resolved + directory_length + 1, app->config.sound_path,
               sound_length + 1);
        memcpy(app->sound_path, resolved, sound_length + directory_length + 2);
    }
}

static void update_input_shape(App *app) {
    XRectangle rectangle;
    if (!app->visible) return;
    rectangle.x = 0;
    rectangle.y = 0;
    rectangle.width = PET_SIZE;
    rectangle.height = PET_SIZE;
    XShapeCombineRectangles(app->display, app->pet, ShapeInput, 0, 0,
                            &rectangle, 1, ShapeSet, Unsorted);
}

static void place_pet(App *app) {
    int x, y;
    int was_visible = app->visible;
    int moved;
    if (!clamp_position(app, app->relative_x, app->relative_y, &app->relative_x, &app->relative_y)) {
        if (app->visible) {
            XUnmapWindow(app->display, app->pet);
            app->visible = 0;
        }
        return;
    }
    x = app->terminal.x + app->relative_x;
    y = app->terminal.y + app->relative_y;
    moved = !was_visible || app->screen_x != x || app->screen_y != y;
    app->screen_x = x;
    app->screen_y = y;
    if (!was_visible) {
        XMoveWindow(app->display, app->pet, x, y);
        XMapRaised(app->display, app->pet);
        app->visible = 1;
    }
    if (moved) {
        if (was_visible) XMoveWindow(app->display, app->pet, x, y);
        update_input_shape(app);
        if (!was_visible) render(app);
        XFlush(app->display);
    }
    // Keep the unmanaged overlay above the active terminal without asking
    // the window manager to activate or focus it.
    XRaiseWindow(app->display, app->pet);
}

static void refresh(App *app) {
    Terminal target;
    if (!find_target(app, &target)) {
        app->has_terminal = 0;
        if (app->visible) {
            XUnmapWindow(app->display, app->pet);
            app->visible = 0;
        }
        return;
    }
    app->terminal = target;
    app->has_terminal = 1;
    place_pet(app);
}

static void start_animation(App *app) {
    app->animation_started = now_ms();
    app->animation_scale = 1.0;
    render(app);
}

static void update_animation(App *app) {
    long long elapsed;
    double progress;
    if (!app->animation_started) return;
    elapsed = now_ms() - app->animation_started;
    if (elapsed >= ANIMATION_MS) {
        app->animation_started = 0;
        app->animation_scale = 1.0;
    } else {
        progress = (double)elapsed / ANIMATION_MS;
        app->animation_scale = progress < 0.4
            ? 1.0 - (progress / 0.4) * 0.28
            : 0.72 + ((progress - 0.4) / 0.6) * 0.28;
    }
    render(app);
}

static void handle_event(App *app, XEvent *event) {
    if (event->type == Expose) {
        render(app);
    } else if (event->type == ButtonPress && event->xbutton.button == Button1) {
        app->dragging = 0;
        app->press_root_x = event->xbutton.x_root;
        app->press_root_y = event->xbutton.y_root;
        app->press_window_x = app->relative_x;
        app->press_window_y = app->relative_y;
        app->press_started = now_ms();
        app->long_press_sent = 0;
        app->bubble_pressed = event->xbutton.x < 112 && event->xbutton.y < 98;
        // Bubble clicks only change which saved display is shown. Character
        // clicks keep the refresh action; neither action changes bubble size.
        if (!app->bubble_pressed) {
            start_animation(app);
            play_sound(app);
        }
    } else if (event->type == ButtonPress && event->xbutton.button == Button3) {
        /* Right click is deliberately action-free: it only gives feedback.
         * In particular, it must never refresh the current API display or
         * cycle the bubble configuration. */
        app->dragging = 0;
        app->press_started = 0;
        app->long_press_sent = 1;
        app->bubble_pressed = 0;
        start_animation(app);
        play_sound(app);
    } else if (event->type == MotionNotify && (event->xmotion.state & Button1Mask)) {
        int dx = event->xmotion.x_root - app->press_root_x;
        int dy = event->xmotion.y_root - app->press_root_y;
        if (abs(dx) > CLICK_SLOP || abs(dy) > CLICK_SLOP) {
            if (!app->dragging) {
                app->dragging = 1;
                app->animation_started = 0;
                app->animation_scale = 1.0;
                render(app);
            }
        }
        if (app->dragging && app->has_terminal) {
            int next_x, next_y;
            if (clamp_position(app, app->press_window_x + dx,
                               app->press_window_y + dy, &next_x, &next_y)) {
                app->relative_x = next_x;
                app->relative_y = next_y;
                app->screen_x = app->terminal.x + next_x;
                app->screen_y = app->terminal.y + next_y;
                // The artwork is already on the server-side window. Moving
                // the override-redirect window is the hot path: no redraw,
                // no full-screen upload, and flush immediately for low drag
                // latency.
                XMoveWindow(app->display, app->pet,
                            app->screen_x, app->screen_y);
                XFlush(app->display);
            }
        }
    } else if (event->type == ButtonRelease && event->xbutton.button == Button1) {
        if (!app->dragging && app->bubble_pressed) switch_bubble_config(app);
        else if (!app->dragging && !app->long_press_sent) start_fetch(app);
        app->dragging = 0;
        app->press_started = 0;
        app->bubble_pressed = 0;
    } else if (event->type == ButtonRelease && event->xbutton.button == Button3) {
        app->press_started = 0;
        app->dragging = 0;
    }
}

static void update_long_press(App *app) {
    if (!app->press_started || app->dragging || app->bubble_pressed || app->long_press_sent)
        return;
    if (now_ms() - app->press_started < 550) return;
    app->long_press_sent = 1;
    app->animation_started = 0;
    app->animation_scale = 1.0;
    render(app);
    open_settings(app);
}

static void set_atom_list_property(App *app, Atom property, Atom *values, int count) {
    XChangeProperty(app->display, app->pet, property, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)values, count);
}

static void set_window_hints(App *app) {
    XClassHint class_hint = {(char *)"codex-pet", (char *)"CodexPet"};
    XWMHints hints;
    Atom type = app->net_wm_window_type_utility;
    Atom states[] = {app->net_wm_state_above, app->net_wm_state_skip_taskbar,
                     app->net_wm_state_skip_pager};
    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint;
    hints.input = False;
    XSetClassHint(app->display, app->pet, &class_hint);
    XSetWMHints(app->display, app->pet, &hints);
    set_atom_list_property(app, app->net_wm_window_type, &type, 1);
    set_atom_list_property(app, app->net_wm_state, states, 3);
    XStoreName(app->display, app->pet, "Codex Pet");
}

static void init_assets(App *app, const char *directory) {
    char pet_path[PATH_MAX];
    char bubble_path[PATH_MAX];
    cairo_status_t pet_status, bubble_status;
    snprintf(pet_path, sizeof(pet_path), "%s/processed_assets/pet_transparent.png", directory);
    snprintf(bubble_path, sizeof(bubble_path), "%s/processed_assets/chat_bubble_transparent.png", directory);
    app->pet_image = cairo_image_surface_create_from_png(pet_path);
    app->bubble_image = cairo_image_surface_create_from_png(bubble_path);
    pet_status = cairo_surface_status(app->pet_image);
    bubble_status = cairo_surface_status(app->bubble_image);
    if (pet_status != CAIRO_STATUS_SUCCESS || bubble_status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "codex-pet: cannot load transparent assets from %s\n", directory);
        exit(2);
    }
}

static void init_x11(App *app, const char *directory) {
    XVisualInfo visual_info;
    XSetWindowAttributes attributes;
    int found_visual;
    app->display = XOpenDisplay(NULL);
    if (!app->display) die("cannot open X11 display");
    XSetErrorHandler(x_error_handler);
    app->screen = DefaultScreen(app->display);
    app->root = RootWindow(app->display, app->screen);
    found_visual = XMatchVisualInfo(app->display, app->screen, 32, TrueColor, &visual_info);
    if (!found_visual) die("no 32-bit ARGB visual available");
    app->visual = visual_info.visual;
    app->colormap = XCreateColormap(app->display, app->root, app->visual, AllocNone);
    memset(&attributes, 0, sizeof(attributes));
    attributes.colormap = app->colormap;
    // A non-default visual cannot inherit the root's border/background
    // values.  Omitting these causes BadMatch on some Xorg/Mutter setups.
    attributes.border_pixel = 0;
    attributes.background_pixel = 0;
    attributes.override_redirect = True;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    app->pet = XCreateWindow(app->display, app->root, 0, 0,
                             PET_SIZE, PET_SIZE, 0,
                             visual_info.depth, InputOutput, app->visual,
                             CWColormap | CWBorderPixel | CWBackPixel |
                             CWOverrideRedirect | CWEventMask, &attributes);
    app->client_list = XInternAtom(app->display, "_NET_CLIENT_LIST", False);
    app->active_window = XInternAtom(app->display, "_NET_ACTIVE_WINDOW", False);
    app->wm_pid = XInternAtom(app->display, "_NET_WM_PID", False);
    app->wm_name = XInternAtom(app->display, "_NET_WM_NAME", False);
    app->utf8_string = XInternAtom(app->display, "UTF8_STRING", False);
    app->net_wm_window_type = XInternAtom(app->display, "_NET_WM_WINDOW_TYPE", False);
    app->net_wm_window_type_utility = XInternAtom(app->display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    app->net_wm_state = XInternAtom(app->display, "_NET_WM_STATE", False);
    app->net_wm_state_above = XInternAtom(app->display, "_NET_WM_STATE_ABOVE", False);
    app->net_wm_state_skip_taskbar = XInternAtom(app->display, "_NET_WM_STATE_SKIP_TASKBAR", False);
    app->net_wm_state_skip_pager = XInternAtom(app->display, "_NET_WM_STATE_SKIP_PAGER", False);
    set_window_hints(app);
    init_assets(app, directory);
    app->x_surface = cairo_xlib_surface_create(app->display, app->pet,
                                               app->visual,
                                               PET_SIZE, PET_SIZE);
    cairo_xlib_surface_set_size(app->x_surface,
                                PET_SIZE, PET_SIZE);
    app->back_buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                   PET_SIZE, PET_SIZE);
    app->animation_scale = 1.0;
    XSelectInput(app->display, app->pet,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
}

static void destroy_app(App *app) {
    if (app->sound_pid > 0) {
        kill(app->sound_pid, SIGTERM);
        waitpid(app->sound_pid, NULL, 0);
    }
    if (app->x_surface) cairo_surface_destroy(app->x_surface);
    if (app->back_buffer) cairo_surface_destroy(app->back_buffer);
    if (app->pet_image) cairo_surface_destroy(app->pet_image);
    if (app->bubble_image) cairo_surface_destroy(app->bubble_image);
    if (app->pet) XDestroyWindow(app->display, app->pet);
    if (app->colormap) XFreeColormap(app->display, app->colormap);
    if (app->display) XCloseDisplay(app->display);
    free(app->codex_pids);
}

int main(int argc, char **argv) {
    char executable[PATH_MAX];
    char directory[PATH_MAX];
    ssize_t executable_length;
    struct pollfd pollfd;
    App app;
    if (argc > 1 && !strcmp(argv[1], "--settings")) return settings_run(argc, argv);
    memset(&app, 0, sizeof(app));
    app.relative_x = 16;
    app.relative_y = 12;
    app.demo = argc > 1 && !strcmp(argv[1], "--demo");
    executable_length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (executable_length <= 0) die("cannot locate executable directory");
    executable[executable_length] = '\0';
    strncpy(directory, executable, sizeof(directory));
    directory[sizeof(directory) - 1] = '\0';
    {
        char *slash = strrchr(directory, '/');
        if (slash) *slash = '\0';
        else strcpy(directory, ".");
    }
    snprintf(app.directory, sizeof(app.directory), "%s", directory);
    snprintf(app.config_path, sizeof(app.config_path), "%s/codex-pet.json", directory);
    pet_config_load(&app.config, app.config_path);
    snprintf(app.audio_device, sizeof(app.audio_device), "%s",
             app.config.audio_device);
    resolve_sound_path(&app, directory);
    config_update_mtime(&app);
    if (app.config.count > 0 && app.config.items[app.config.active].value[0])
        snprintf(app.bubble_text, sizeof(app.bubble_text), "%s",
                 app.config.items[app.config.active].value);
    init_x11(&app, directory);
    pollfd.fd = ConnectionNumber(app.display);
    pollfd.events = POLLIN;
    for (;;) {
        int timeout = app.animation_started ? 16 : POLL_MS;
        int result = poll(&pollfd, 1, timeout);
        if (result > 0 && (pollfd.revents & POLLIN)) {
            while (XPending(app.display)) {
                XEvent event;
                XNextEvent(app.display, &event);
                if (event.type == MotionNotify) {
                    // Discard stale pointer samples when the compositor or
                    // X socket delivers a burst. Only the newest position is
                    // useful for a drag.
                    XEvent newer;
                    while (XCheckTypedWindowEvent(app.display, app.pet,
                                                  MotionNotify, &newer)) {
                        event = newer;
                    }
                }
                handle_event(&app, &event);
            }
        }
        update_long_press(&app);
        update_animation(&app);
        complete_fetch(&app);
        complete_sound(&app);
        reload_config_if_changed(&app);
        refresh(&app);
    }
    destroy_app(&app);
    return 0;
}
