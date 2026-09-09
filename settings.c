#include "config.h"
#include "settings.h"

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

enum { COLUMN_INDEX, COLUMN_NAME, COLUMN_COUNT };

typedef struct {
    PetConfig config;
    int selected;
    int loading;
    GtkListStore *store;
    GtkTreeSelection *selection;
    GtkWidget *name_entry;
    GtkWidget *curl_entry;
    GtkWidget *json_entry;
    GtkWidget *template_entry;
    GtkWidget *sound_entry;
    GtkWidget *active_label;
    GtkWidget *status;
} Settings;

static void set_status(Settings *settings, const char *message) {
    gtk_label_set_text(GTK_LABEL(settings->status), message);
}

static void copy_entry(char *destination, size_t capacity, GtkWidget *entry) {
    snprintf(destination, capacity, "%s", gtk_entry_get_text(GTK_ENTRY(entry)));
}

static void save_form(Settings *settings) {
    PetConfigItem *item;
    if (settings->loading || settings->selected < 0 ||
        settings->selected >= settings->config.count) return;
    item = &settings->config.items[settings->selected];
    copy_entry(item->name, sizeof(item->name), settings->name_entry);
    copy_entry(item->curl, sizeof(item->curl), settings->curl_entry);
    copy_entry(item->json_path, sizeof(item->json_path), settings->json_entry);
    copy_entry(item->template_text, sizeof(item->template_text), settings->template_entry);
}

static void save_sound(Settings *settings) {
    if (!settings->loading && settings->sound_entry)
        copy_entry(settings->config.sound_path, sizeof(settings->config.sound_path),
                   settings->sound_entry);
}

static void on_sound_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    save_sound(user_data);
}

static void load_form(Settings *settings) {
    PetConfigItem *item;
    settings->loading = 1;
    if (settings->selected < 0 || settings->selected >= settings->config.count) {
        gtk_entry_set_text(GTK_ENTRY(settings->name_entry), "");
        gtk_entry_set_text(GTK_ENTRY(settings->curl_entry), "");
        gtk_entry_set_text(GTK_ENTRY(settings->json_entry), "");
        gtk_entry_set_text(GTK_ENTRY(settings->template_entry), "");
    } else {
        item = &settings->config.items[settings->selected];
        gtk_entry_set_text(GTK_ENTRY(settings->name_entry), item->name);
        gtk_entry_set_text(GTK_ENTRY(settings->curl_entry), item->curl);
        gtk_entry_set_text(GTK_ENTRY(settings->json_entry), item->json_path);
        gtk_entry_set_text(GTK_ENTRY(settings->template_entry), item->template_text);
    }
    gtk_entry_set_text(GTK_ENTRY(settings->sound_entry), settings->config.sound_path);
    settings->loading = 0;
}

static void select_index(Settings *settings, int index) {
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(settings->store);
    if (index < 0 || index >= settings->config.count) {
        settings->selected = -1;
        load_form(settings);
        return;
    }
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            int value;
            gtk_tree_model_get(model, &iter, COLUMN_INDEX, &value, -1);
            if (value == index) {
                GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
                /* Selecting a row emits "changed" synchronously. Suppress
                 * that callback until the corresponding old values are in
                 * the form; otherwise the empty widgets overwrite the item
                 * during the initial window construction. */
                settings->loading = 1;
                gtk_tree_selection_select_path(settings->selection, path);
                settings->loading = 0;
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(gtk_tree_selection_get_tree_view(settings->selection)),
                                             path, NULL, FALSE, 0, 0);
                gtk_tree_path_free(path);
                settings->selected = index;
                load_form(settings);
                return;
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    settings->selected = -1;
    load_form(settings);
}

static void refresh_list(Settings *settings) {
    GtkTreeIter iter;
    int i;
    settings->loading = 1;
    gtk_list_store_clear(settings->store);
    for (i = 0; i < settings->config.count; i++) {
        gtk_list_store_append(settings->store, &iter);
        gtk_list_store_set(settings->store, &iter,
                           COLUMN_INDEX, i, COLUMN_NAME,
                           settings->config.items[i].name, -1);
    }
    settings->loading = 0;
    if (settings->config.count > 0) {
        if (settings->selected < 0 || settings->selected >= settings->config.count)
            settings->selected = settings->config.active;
        select_index(settings, settings->selected);
    } else {
        settings->selected = -1;
        load_form(settings);
    }
}

static void on_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
    Settings *settings = user_data;
    GtkTreeModel *model;
    GtkTreeIter iter;
    int index;
    if (settings->loading || !gtk_tree_selection_get_selected(selection, &model, &iter)) return;
    save_form(settings);
    gtk_tree_model_get(model, &iter, COLUMN_INDEX, &index, -1);
    settings->selected = index;
    load_form(settings);
}

static void on_form_changed(GtkEditable *editable, gpointer user_data) {
    (void)editable;
    save_form(user_data);
}

static void on_add_clicked(GtkButton *button, gpointer user_data) {
    Settings *settings = user_data;
    PetConfigItem *item;
    (void)button;
    save_form(settings);
    save_sound(settings);
    if (settings->config.count >= PET_CONFIG_MAX_ITEMS) return;
    item = &settings->config.items[settings->config.count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->name, sizeof(item->name), "显示 %d", settings->config.count);
    snprintf(item->template_text, sizeof(item->template_text), "$content");
    settings->selected = settings->config.count - 1;
    refresh_list(settings);
}

static void on_delete_clicked(GtkButton *button, gpointer user_data) {
    Settings *settings = user_data;
    int i;
    (void)button;
    save_form(settings);
    if (settings->selected < 0 || settings->selected >= settings->config.count) return;
    for (i = settings->selected; i + 1 < settings->config.count; i++)
        settings->config.items[i] = settings->config.items[i + 1];
    settings->config.count--;
    if (settings->config.active >= settings->config.count) settings->config.active = 0;
    if (settings->selected >= settings->config.count) settings->selected = settings->config.count - 1;
    refresh_list(settings);
}

static void refresh_active_label(Settings *settings) {
    char text[PET_CONFIG_NAME_MAX + 64];
    if (!settings->active_label) return;
    if (settings->config.count > 0 && settings->config.active >= 0 &&
        settings->config.active < settings->config.count)
        snprintf(text, sizeof(text), "当前配置：%s（点击气泡切换）",
                 settings->config.items[settings->config.active].name);
    else snprintf(text, sizeof(text), "当前配置：无（点击气泡切换）");
    gtk_label_set_text(GTK_LABEL(settings->active_label), text);
}

static void on_save_clicked(GtkButton *button, gpointer user_data) {
    Settings *settings = user_data;
    (void)button;
    save_form(settings);
    save_sound(settings);
    if (pet_config_save(&settings->config)) set_status(settings, "已保存到 codex-pet.json");
    else set_status(settings, "保存失败，请检查 JSON 文件权限");
    refresh_active_label(settings);
    refresh_list(settings);
}

static GtkWidget *make_entry(const char *placeholder) {
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_hexpand(entry, TRUE);
    return entry;
}

static void add_labeled_entry(GtkGrid *grid, int row, const char *label_text,
                              GtkWidget *entry, const char *help) {
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, entry, 1, row, 1, 1);
    if (help) {
        GtkWidget *hint = gtk_label_new(help);
        gtk_widget_set_halign(hint, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
        gtk_grid_attach(grid, hint, 1, row + 1, 1, 1);
    }
}

static GtkWidget *build_window(Settings *settings) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scrolled;
    GtkWidget *tree;
    GtkWidget *button_row;
    GtkWidget *add_button;
    GtkWidget *delete_button;
    GtkWidget *save_button;
    GtkWidget *close_button;
    GtkWidget *grid;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkWidget *active_row;
    gtk_window_set_title(GTK_WINDOW(window), "Codex Pet · 气泡显示配置");
    gtk_window_set_default_size(GTK_WINDOW(window), 820, 480);
    gtk_container_set_border_width(GTK_CONTAINER(root), 14);
    gtk_container_add(GTK_CONTAINER(window), root);
    gtk_box_pack_start(GTK_BOX(root), content, TRUE, TRUE, 0);
    gtk_widget_set_size_request(left, 210, -1);
    gtk_box_pack_start(GTK_BOX(content), left, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), right, TRUE, TRUE, 0);

    settings->store = gtk_list_store_new(COLUMN_COUNT, G_TYPE_INT, G_TYPE_STRING);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(settings->store));
    settings->selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(settings->selection, GTK_SELECTION_SINGLE);
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("显示项目", renderer, "text", COLUMN_NAME, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    g_signal_connect(settings->selection, "changed", G_CALLBACK(on_selection_changed), settings);
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled), tree);
    gtk_box_pack_start(GTK_BOX(left), scrolled, TRUE, TRUE, 0);
    button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    add_button = gtk_button_new_with_label("新增");
    delete_button = gtk_button_new_with_label("删除");
    gtk_box_pack_start(GTK_BOX(button_row), add_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), delete_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(left), button_row, FALSE, FALSE, 0);
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_clicked), settings);
    g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_clicked), settings);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    settings->name_entry = make_entry("余额");
    settings->curl_entry = make_entry("curl -s 'https://...'");
    settings->json_entry = make_entry("data.balance 或 {\"data\":{\"balance\":$content}}");
    settings->template_entry = make_entry("余额：$content");
    settings->sound_entry = make_entry("assets/Ya1.mp3");
    add_labeled_entry(GTK_GRID(grid), 0, "显示名称", settings->name_entry, NULL);
    add_labeled_entry(GTK_GRID(grid), 1, "curl 指令", settings->curl_entry, NULL);
    add_labeled_entry(GTK_GRID(grid), 2, "JSON 解析", settings->json_entry, "留空显示完整返回；支持 data.balance 或 JSON 模板");
    add_labeled_entry(GTK_GRID(grid), 4, "显示模板", settings->template_entry, "$content 会替换成解析后的值");
    add_labeled_entry(GTK_GRID(grid), 5, "点击音效", settings->sound_entry, "支持绝对路径或相对于程序目录的路径；留空关闭音效");
    gtk_box_pack_start(GTK_BOX(right), grid, FALSE, FALSE, 0);
    g_signal_connect(settings->name_entry, "changed", G_CALLBACK(on_form_changed), settings);
    g_signal_connect(settings->curl_entry, "changed", G_CALLBACK(on_form_changed), settings);
    g_signal_connect(settings->json_entry, "changed", G_CALLBACK(on_form_changed), settings);
    g_signal_connect(settings->template_entry, "changed", G_CALLBACK(on_form_changed), settings);
    g_signal_connect(settings->sound_entry, "changed", G_CALLBACK(on_sound_changed), settings);

    active_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(right), active_row, FALSE, FALSE, 10);
    settings->active_label = gtk_label_new("");
    gtk_widget_set_halign(settings->active_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(active_row), settings->active_label, TRUE, TRUE, 0);

    settings->status = gtk_label_new("修改后点击保存");
    gtk_widget_set_halign(settings->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right), settings->status, FALSE, FALSE, 0);
    button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    save_button = gtk_button_new_with_label("保存");
    close_button = gtk_button_new_with_label("关闭");
    gtk_box_pack_end(GTK_BOX(button_row), close_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(button_row), save_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(right), button_row, FALSE, FALSE, 0);
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_save_clicked), settings);
    g_signal_connect_swapped(close_button, "clicked", G_CALLBACK(gtk_widget_destroy), window);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    refresh_active_label(settings);
    refresh_list(settings);
    return window;
}

int settings_run(int argc, char **argv) {
    Settings settings;
    GtkWidget *window;
    const char *path = argc > 2 ? argv[2] : "codex-pet.json";
    memset(&settings, 0, sizeof(settings));
    settings.selected = -1;
    pet_config_load(&settings.config, path);
    gtk_init(&argc, &argv);
    window = build_window(&settings);
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
