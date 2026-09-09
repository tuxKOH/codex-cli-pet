#define _GNU_SOURCE

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/wait.h>

static char *trim(char *text) {
    char *end;
    while (*text && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static void copy_text(char *destination, size_t capacity, const char *source) {
    if (!capacity) return;
    snprintf(destination, capacity, "%s", source ? source : "");
}

static void strip_quotes(char *value) {
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                        (value[0] == '\'' && value[length - 1] == '\''))) {
        memmove(value, value + 1, length - 2);
        value[length - 2] = '\0';
    }
}

static PetConfigItem *ensure_item(PetConfig *config, int index) {
    PetConfigItem *item;
    if (index < 0 || index >= PET_CONFIG_MAX_ITEMS) return NULL;
    while (config->count <= index) {
        item = &config->items[config->count++];
        memset(item, 0, sizeof(*item));
        snprintf(item->name, sizeof(item->name), "显示 %d", config->count);
        copy_text(item->template_text, sizeof(item->template_text), "$content");
    }
    return &config->items[index];
}

static int suffix_index(const char *key, const char *prefix) {
    const char *suffix;
    char *end;
    long value;
    if (strncmp(key, prefix, strlen(prefix)) != 0) return -1;
    suffix = key + strlen(prefix);
    if (!*suffix) return -1;
    value = strtol(suffix, &end, 10);
    if (*end || value < 0 || value >= PET_CONFIG_MAX_ITEMS) return -1;
    return (int)value;
}

/* The configuration file is deliberately parsed here instead of depending on
 * a JSON library: this program already has a small JSON path reader below,
 * and keeping the settings file self-contained makes installation simple. */
static const char *cfg_skip_space(const char *cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static const char *cfg_skip_string(const char *cursor) {
    if (*cursor != '"') return NULL;
    cursor++;
    while (*cursor) {
        if (*cursor == '\\' && cursor[1]) cursor += 2;
        else if (*cursor++ == '"') return cursor;
    }
    return NULL;
}

static const char *cfg_skip_value(const char *cursor) {
    int depth = 0;
    char open, close;
    cursor = cfg_skip_space(cursor);
    if (*cursor == '"') return cfg_skip_string(cursor);
    if (*cursor != '{' && *cursor != '[') {
        while (*cursor && *cursor != ',' && *cursor != '}' && *cursor != ']') cursor++;
        return cursor;
    }
    open = *cursor;
    close = open == '{' ? '}' : ']';
    do {
        if (*cursor == '"') cursor = cfg_skip_string(cursor);
        else {
            if (*cursor == open) depth++;
            if (*cursor == close) depth--;
            cursor++;
        }
    } while (*cursor && depth > 0);
    return depth == 0 ? cursor : NULL;
}

static const char *cfg_parse_string(const char *cursor, char *output, size_t capacity) {
    size_t used = 0;
    if (!cursor || *cursor != '"') return NULL;
    cursor++;
    while (*cursor && *cursor != '"') {
        unsigned value = 0;
        if (*cursor == '\\') {
            cursor++;
            if (!*cursor) return NULL;
            switch (*cursor) {
            case '"': case '\\': case '/': value = (unsigned char)*cursor; cursor++; break;
            case 'b': value = '\b'; cursor++; break;
            case 'f': value = '\f'; cursor++; break;
            case 'n': value = '\n'; cursor++; break;
            case 'r': value = '\r'; cursor++; break;
            case 't': value = '\t'; cursor++; break;
            case 'u':
                if (!isxdigit((unsigned char)cursor[1]) ||
                    !isxdigit((unsigned char)cursor[2]) ||
                    !isxdigit((unsigned char)cursor[3]) ||
                    !isxdigit((unsigned char)cursor[4])) return NULL;
                sscanf(cursor + 1, "%4x", &value);
                cursor += 5;
                /* ASCII escapes are enough for hand-written files. UTF-8
                 * entered by the GTK editor is saved as literal UTF-8. */
                if (value > 0x7f) value = '?';
                break;
            default: return NULL;
            }
        } else value = (unsigned char)*cursor++;
        if (used + 1 < capacity) output[used++] = (char)value;
    }
    if (*cursor != '"') return NULL;
    if (capacity) output[used < capacity ? used : capacity - 1] = '\0';
    return cursor + 1;
}

static const char *cfg_parse_integer(const char *cursor, int *output) {
    char *end;
    long value;
    cursor = cfg_skip_space(cursor);
    errno = 0;
    value = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE) return NULL;
    *output = (int)value;
    return end;
}

static int load_json(PetConfig *config, const char *text) {
    const char *cursor = cfg_skip_space(text);
    char key[PET_CONFIG_NAME_MAX];
    if (*cursor++ != '{') return 0;
    for (;;) {
        cursor = cfg_skip_space(cursor);
        if (*cursor == '}') break;
        if (*cursor != '"') return 0;
        cursor = cfg_parse_string(cursor, key, sizeof(key));
        if (!cursor || *cfg_skip_space(cursor) != ':') return 0;
        cursor = cfg_skip_space(cursor) + 1;
        cursor = cfg_skip_space(cursor);
        if (!strcmp(key, "active")) {
            cursor = cfg_parse_integer(cursor, &config->active);
            if (!cursor) return 0;
        } else if (!strcmp(key, "displays")) {
            cursor = cfg_skip_space(cursor);
            if (*cursor++ != '[') return 0;
            for (;;) {
                PetConfigItem *item;
                cursor = cfg_skip_space(cursor);
                if (*cursor == ']') { cursor++; break; }
                if (*cursor++ != '{' || config->count >= PET_CONFIG_MAX_ITEMS) return 0;
                item = ensure_item(config, config->count);
                for (;;) {
                    cursor = cfg_skip_space(cursor);
                    if (*cursor == '}') { cursor++; break; }
                    if (*cursor != '"') return 0;
                    cursor = cfg_parse_string(cursor, key, sizeof(key));
                    if (!cursor || *cfg_skip_space(cursor) != ':') return 0;
                    cursor = cfg_skip_space(cursor) + 1;
                    cursor = cfg_skip_space(cursor);
                    if (!strcmp(key, "name") || !strcmp(key, "curl") ||
                        !strcmp(key, "json") || !strcmp(key, "template") ||
                        !strcmp(key, "value")) {
                        char *destination = !strcmp(key, "name") ? item->name :
                            !strcmp(key, "curl") ? item->curl :
                            !strcmp(key, "json") ? item->json_path :
                            !strcmp(key, "template") ? item->template_text : item->value;
                        size_t capacity = !strcmp(key, "name") ? sizeof(item->name) :
                            !strcmp(key, "curl") ? sizeof(item->curl) :
                            !strcmp(key, "json") ? sizeof(item->json_path) :
                            !strcmp(key, "template") ? sizeof(item->template_text) : sizeof(item->value);
                        cursor = cfg_parse_string(cursor, destination, capacity);
                        if (!cursor) return 0;
                    } else {
                        cursor = cfg_skip_value(cursor);
                        if (!cursor) return 0;
                    }
                    cursor = cfg_skip_space(cursor);
                    if (*cursor == ',') cursor++;
                    else if (*cursor != '}') return 0;
                }
                cursor = cfg_skip_space(cursor);
                if (*cursor == ',') cursor++;
                else if (*cursor != ']') return 0;
            }
        } else {
            cursor = cfg_skip_value(cursor);
            if (!cursor) return 0;
        }
        cursor = cfg_skip_space(cursor);
        if (*cursor == ',') cursor++;
        else if (*cursor != '}') return 0;
    }
    if (config->count > 0 && (config->active < 0 || config->active >= config->count)) config->active = 0;
    return 1;
}

static int load_env(PetConfig *config, const char *path) {
    FILE *file;
    char line[4096];
    int configured_count = -1;
    int generic_curl = 0;
    char generic_curl_value[PET_CONFIG_CURL_MAX] = "";
    char generic_json[PET_CONFIG_JSON_MAX] = "";
    char generic_template[PET_CONFIG_TEMPLATE_MAX] = "";
    file = fopen(path, "r");
    if (!file) return 0;
    while (fgets(line, sizeof(line), file)) {
        char *key, *value;
        int index;
        line[strcspn(line, "\r\n")] = '\0';
        key = trim(line);
        if (!*key || *key == '#') continue;
        if (!strncmp(key, "export ", 7)) key = trim(key + 7);
        value = strchr(key, '=');
        if (!value) continue;
        *value++ = '\0'; key = trim(key); value = trim(value); strip_quotes(value);
        if (!strcmp(key, "CODEX_PET_COUNT")) configured_count = atoi(value);
        else if (!strcmp(key, "CODEX_PET_ACTIVE")) config->active = atoi(value);
        else if (!strcmp(key, "CODEX_PET_CURL") || !strcmp(key, "CODEX_CURL") ||
                 !strcmp(key, "NEWAPI_CURL") || !strcmp(key, "CURL_COMMAND")) {
            copy_text(generic_curl_value, sizeof(generic_curl_value), value); generic_curl = 1;
        } else if (!strcmp(key, "CODEX_PET_JSON")) copy_text(generic_json, sizeof(generic_json), value);
        else if (!strcmp(key, "CODEX_PET_TEMPLATE")) copy_text(generic_template, sizeof(generic_template), value);
        else if ((index = suffix_index(key, "CODEX_PET_NAME_")) >= 0)
            copy_text(ensure_item(config, index)->name, PET_CONFIG_NAME_MAX, value);
        else if ((index = suffix_index(key, "CODEX_PET_CURL_")) >= 0)
            copy_text(ensure_item(config, index)->curl, PET_CONFIG_CURL_MAX, value);
        else if ((index = suffix_index(key, "CODEX_PET_JSON_")) >= 0)
            copy_text(ensure_item(config, index)->json_path, PET_CONFIG_JSON_MAX, value);
        else if ((index = suffix_index(key, "CODEX_PET_TEMPLATE_")) >= 0)
            copy_text(ensure_item(config, index)->template_text, PET_CONFIG_TEMPLATE_MAX, value);
    }
    fclose(file);
    if (configured_count > 0 && configured_count < PET_CONFIG_MAX_ITEMS)
        while (config->count < configured_count) ensure_item(config, config->count);
    if (generic_curl) {
        PetConfigItem *item = ensure_item(config, 0);
        copy_text(item->curl, sizeof(item->curl), generic_curl_value);
        if (generic_json[0]) copy_text(item->json_path, sizeof(item->json_path), generic_json);
        if (generic_template[0]) copy_text(item->template_text, sizeof(item->template_text), generic_template);
    }
    if (config->count > 0 && (config->active < 0 || config->active >= config->count)) config->active = 0;
    return 1;
}

static int legacy_path_for(const char *json_path, char *legacy, size_t capacity) {
    const char *slash = strrchr(json_path, '/');
    size_t directory_length = slash ? (size_t)(slash - json_path) : 0;
    if (directory_length + 6 >= capacity) return 0;
    if (slash) {
        memcpy(legacy, json_path, directory_length);
        legacy[directory_length] = '/';
        strcpy(legacy + directory_length + 1, ".env");
    } else strcpy(legacy, ".env");
    return 1;
}

static int ends_with(const char *text, const char *suffix) {
    size_t text_length = strlen(text), suffix_length = strlen(suffix);
    return text_length >= suffix_length && !strcmp(text + text_length - suffix_length, suffix);
}

void pet_config_init(PetConfig *config, const char *path) {
    memset(config, 0, sizeof(*config));
    copy_text(config->path, sizeof(config->path), path ? path : ".env");
    config->active = 0;
}

int pet_config_load(PetConfig *config, const char *path) {
    FILE *file;
    char *text;
    long length;
    char legacy[4096];
    pet_config_init(config, path);
    file = fopen(config->path, "r");
    if (file) {
        fseek(file, 0, SEEK_END); length = ftell(file); rewind(file);
        if (length < 0 || length > 1024 * 1024) { fclose(file); return 0; }
        text = malloc((size_t)length + 1);
        if (!text) { fclose(file); return 0; }
        if (fread(text, 1, (size_t)length, file) != (size_t)length) { free(text); fclose(file); return 0; }
        text[length] = '\0'; fclose(file);
        if (load_json(config, text)) { free(text); return 1; }
        free(text);
        /* A path explicitly ending in .env is still accepted for compatibility. */
        if (ends_with(config->path, ".env"))
            return load_env(config, config->path);
        return 0;
    }
    if (legacy_path_for(config->path, legacy, sizeof(legacy)) && load_env(config, legacy)) {
        /* Keep the old file readable, but make the new JSON file authoritative
         * from this point onward. */
        pet_config_save(config);
        return 1;
    }
    return 0;
}

int pet_config_save(const PetConfig *config) {
    FILE *file = fopen(config->path, "w");
    int i;
    if (!file) return 0;
    fputs("{\n  \"active\": ", file);
    fprintf(file, "%d,\n  \"displays\": [\n", config->active);
    for (i = 0; i < config->count; i++) {
        const PetConfigItem *item = &config->items[i];
        const char *values[] = {item->name, item->curl, item->json_path, item->template_text, item->value};
        const char *keys[] = {"name", "curl", "json", "template", "value"};
        int j;
        fprintf(file, "    { ");
        for (j = 0; j < 5; j++) {
            const unsigned char *cursor = (const unsigned char *)values[j];
            fprintf(file, "\"%s\":\"", keys[j]);
            while (*cursor) {
                switch (*cursor) {
                case '\\': fputs("\\\\", file); break;
                case '"': fputs("\\\"", file); break;
                case '\n': fputs("\\n", file); break;
                case '\r': fputs("\\r", file); break;
                case '\t': fputs("\\t", file); break;
                default:
                    if (*cursor < 0x20) fprintf(file, "\\u%04x", *cursor);
                    else fputc(*cursor, file);
                }
                cursor++;
            }
            fprintf(file, "\"%s", j == 4 ? "" : ", ");
        }
        fprintf(file, " }%s\n", i + 1 == config->count ? "" : ",");
    }
    fputs("  ]\n}\n", file);
    if (fclose(file) != 0) return 0;
    return 1;
}

int pet_config_switch(PetConfig *config) {
    if (!config || config->count <= 0) return 0;
    config->active = (config->active + 1) % config->count;
    return pet_config_save(config);
}

int pet_config_fetch_text(const PetConfigItem *item, char *output, size_t output_size) {
    FILE *pipe;
    size_t used = 0;
    int status;
    if (!item->curl[0] || output_size < 2) return 0;
    pipe = popen(item->curl, "r");
    if (!pipe) return 0;
    while (used + 1 < output_size) {
        size_t got = fread(output + used, 1, output_size - used - 1, pipe);
        used += got;
        if (!got || feof(pipe)) break;
    }
    output[used] = '\0';
    status = pclose(pipe);
    return used > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

typedef struct {
    const char *cursor;
    double content;
    int valid;
} Expression;

static void expression_space(Expression *expression) {
    while (*expression->cursor && isspace((unsigned char)*expression->cursor)) expression->cursor++;
}

static double expression_add(Expression *expression);

static double expression_factor(Expression *expression) {
    char *end;
    double value;
    expression_space(expression);
    if (*expression->cursor == '+') { expression->cursor++; return expression_factor(expression); }
    if (*expression->cursor == '-') { expression->cursor++; return -expression_factor(expression); }
    if (*expression->cursor == '(') {
        expression->cursor++;
        value = expression_add(expression);
        expression_space(expression);
        if (*expression->cursor != ')') expression->valid = 0;
        else expression->cursor++;
        return value;
    }
    if (!strncmp(expression->cursor, "$content", 8)) {
        expression->cursor += 8;
        return expression->content;
    }
    value = strtod(expression->cursor, &end);
    if (end == expression->cursor) expression->valid = 0;
    else expression->cursor = end;
    return value;
}

static double expression_term(Expression *expression) {
    double value = expression_factor(expression);
    while (expression->valid) {
        double rhs;
        int operation;
        expression_space(expression);
        if (*expression->cursor == '*') operation = '*';
        else if (*expression->cursor == '/') {
            operation = '/'; expression->cursor++;
            if (*expression->cursor == '/') operation = 'D';
            else { rhs = expression_factor(expression); goto divide; }
        } else if (*expression->cursor == '%') operation = '%';
        else break;
        expression->cursor++;
        rhs = expression_factor(expression);
        if (!expression->valid) break;
        if (operation == '*') value *= rhs;
        else if (operation == '%') {
            if (rhs == 0) expression->valid = 0;
            else value = fmod(value, rhs);
        } else if (operation == 'D') {
            if (rhs == 0) expression->valid = 0;
            else value = floor(value / rhs);
        }
        continue;
divide:
        if (!expression->valid) break;
        if (rhs == 0) expression->valid = 0;
        else value /= rhs;
    }
    return value;
}

static double expression_add(Expression *expression) {
    double value = expression_term(expression);
    while (expression->valid) {
        double rhs;
        expression_space(expression);
        if (*expression->cursor == '+') {
            expression->cursor++; rhs = expression_term(expression); value += rhs;
        } else if (*expression->cursor == '-') {
            expression->cursor++; rhs = expression_term(expression); value -= rhs;
        } else break;
    }
    return value;
}

static int evaluate_expression(const char *text, double content, char *output, size_t output_size) {
    Expression expression = {text, content, 1};
    double value = expression_add(&expression);
    expression_space(&expression);
    if (!expression.valid || *expression.cursor || !isfinite(value)) return 0;
    if (fabs(value - round(value)) < 0.000000001)
        snprintf(output, output_size, "%.0f", value);
    else snprintf(output, output_size, "%.10g", value);
    return output[0] != '\0';
}

static const char *skip_space(const char *cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static const char *skip_json_string(const char *cursor) {
    if (*cursor != '"') return cursor;
    cursor++;
    while (*cursor) {
        if (*cursor == '\\' && cursor[1]) cursor += 2;
        else if (*cursor++ == '"') break;
    }
    return cursor;
}

static const char *skip_json_value(const char *cursor) {
    int depth = 0;
    cursor = skip_space(cursor);
    if (*cursor == '"') return skip_json_string(cursor);
    if (*cursor == '{' || *cursor == '[') {
        char open = *cursor;
        char close = open == '{' ? '}' : ']';
        do {
            if (*cursor == '"') cursor = skip_json_string(cursor);
            else {
                if (*cursor == open) depth++;
                if (*cursor == close) depth--;
                cursor++;
            }
        } while (*cursor && depth > 0);
        return cursor;
    }
    while (*cursor && *cursor != ',' && *cursor != '}' && *cursor != ']' &&
           !isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static int json_key_equals(const char *start, const char *end, const char *key) {
    char decoded[PET_CONFIG_NAME_MAX];
    size_t used = 0;
    start++;
    while (start < end && used + 1 < sizeof(decoded)) {
        if (*start == '\\' && start + 1 < end) start++;
        decoded[used++] = *start++;
    }
    decoded[used] = '\0';
    return !strcmp(decoded, key);
}

static const char *find_json_key(const char *object, const char *key) {
    const char *cursor = skip_space(object);
    if (*cursor != '{') return NULL;
    cursor++;
    for (;;) {
        const char *key_start;
        const char *key_end;
        const char *value;
        cursor = skip_space(cursor);
        if (*cursor == '}') return NULL;
        if (*cursor != '"') return NULL;
        key_start = cursor;
        key_end = skip_json_string(cursor);
        cursor = skip_space(key_end);
        if (*cursor++ != ':') return NULL;
        value = skip_space(cursor);
        if (json_key_equals(key_start, key_end - 1, key)) return value;
        cursor = skip_json_value(value);
        cursor = skip_space(cursor);
        if (*cursor == ',') cursor++;
        else if (*cursor == '}') return NULL;
        else return NULL;
    }
}

static void copy_json_scalar(const char *value, char *output, size_t output_size) {
    const char *end;
    size_t length;
    value = skip_space(value);
    if (*value == '"') {
        end = skip_json_string(value);
        value++;
        end--;
    } else {
        end = skip_json_value(value);
        while (end > value && isspace((unsigned char)end[-1])) end--;
    }
    length = (size_t)(end - value);
    if (length >= output_size) length = output_size - 1;
    memcpy(output, value, length);
    output[length] = '\0';
}

static int pattern_to_path(const char *pattern, char *path, size_t path_size) {
    char keys[16][PET_CONFIG_NAME_MAX];
    int depth = 0;
    const char *cursor = pattern;
    size_t used = 0;
    path[0] = '\0';
    while (*cursor) {
        if (*cursor == '"') {
            const char *end = skip_json_string(cursor);
            const char *after = skip_space(end);
            if (*after == ':') {
                const char *value = skip_space(after + 1);
                char key[PET_CONFIG_NAME_MAX];
                size_t length = (size_t)(end - cursor - 2);
                if (length >= sizeof(key)) length = sizeof(key) - 1;
                memcpy(key, cursor + 1, length);
                key[length] = '\0';
                if (*value == '{' && depth < (int)(sizeof(keys) / sizeof(keys[0]))) {
                    copy_text(keys[depth++], sizeof(keys[0]), key);
                } else if (!strncmp(value, "$content", 8)) {
                    int i;
                    for (i = 0; i < depth; i++) {
                        if (used + strlen(keys[i]) + 2 >= path_size) return 0;
                        if (used) path[used++] = '.';
                        strcpy(path + used, keys[i]);
                        used += strlen(keys[i]);
                    }
                    if (used + strlen(key) + 2 >= path_size) return 0;
                    if (used) path[used++] = '.';
                    strcpy(path + used, key);
                    return 1;
                }
            }
            cursor = end;
        } else {
            if (*cursor == '}' && depth > 0) depth--;
            cursor++;
        }
    }
    return 0;
}

static void extract_content(const PetConfigItem *item, const char *raw,
                            char *content, size_t content_size) {
    char path[PET_CONFIG_JSON_MAX];
    char segment[PET_CONFIG_NAME_MAX];
    const char *cursor;
    const char *value;
    const char *dot;
    if (!item->json_path[0]) {
        copy_json_scalar(raw, content, content_size);
        return;
    }
    if (strstr(item->json_path, "$content")) {
        if (!pattern_to_path(item->json_path, path, sizeof(path))) {
            copy_text(content, content_size, raw);
            return;
        }
    } else copy_text(path, sizeof(path), item->json_path);
    cursor = raw;
    while (*path) {
        dot = strchr(path, '.');
        if (dot) {
            size_t length = (size_t)(dot - path);
            if (length >= sizeof(segment)) length = sizeof(segment) - 1;
            memcpy(segment, path, length);
            segment[length] = '\0';
            memmove(path, dot + 1, strlen(dot + 1) + 1);
        } else {
            copy_text(segment, sizeof(segment), path);
            path[0] = '\0';
        }
        value = find_json_key(cursor, segment);
        if (!value) {
            copy_text(content, content_size, raw);
            return;
        }
        cursor = value;
    }
    copy_json_scalar(cursor, content, content_size);
}

int pet_config_format_value(const PetConfigItem *item, const char *raw_json,
                            char *output, size_t output_size) {
    char content[PET_CONFIG_VALUE_MAX];
    const char *cursor;
    size_t used = 0;
    double content_number = 0;
    char *content_end;
    extract_content(item, raw_json, content, sizeof(content));
    content_number = strtod(content, &content_end);
    if (content_end == content) content_number = 0;
    if (!item->template_text[0]) copy_text(output, output_size, content);
    else {
        cursor = item->template_text;
        while (*cursor && used + 1 < output_size) {
            if (*cursor == '{') {
                const char *end = strchr(cursor + 1, '}');
                char evaluated[64];
                if (end) {
                    size_t length = (size_t)(end - cursor - 1);
                    char expression[PET_CONFIG_TEMPLATE_MAX];
                    if (length >= sizeof(expression)) length = sizeof(expression) - 1;
                    memcpy(expression, cursor + 1, length);
                    expression[length] = '\0';
                    if (evaluate_expression(expression, content_number,
                                             evaluated, sizeof(evaluated))) {
                        size_t value_length = strlen(evaluated);
                        if (value_length > output_size - used - 1)
                            value_length = output_size - used - 1;
                        memcpy(output + used, evaluated, value_length);
                        used += value_length;
                        cursor = end + 1;
                        continue;
                    }
                }
            }
            if (!strncmp(cursor, "$content", 8)) {
                size_t length = strlen(content);
                if (length > output_size - used - 1) length = output_size - used - 1;
                memcpy(output + used, content, length);
                used += length;
                cursor += 8;
            } else output[used++] = *cursor++;
        }
        output[used] = '\0';
    }
    return output[0] != '\0';
}
