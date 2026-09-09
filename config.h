#ifndef CODEX_PET_CONFIG_H
#define CODEX_PET_CONFIG_H

#include <stddef.h>

#define PET_CONFIG_MAX_ITEMS 64
#define PET_CONFIG_NAME_MAX 128
#define PET_CONFIG_CURL_MAX 2048
#define PET_CONFIG_JSON_MAX 512
#define PET_CONFIG_TEMPLATE_MAX 512
#define PET_CONFIG_VALUE_MAX 2048
#define PET_CONFIG_SOUND_MAX 4096

typedef struct {
    char name[PET_CONFIG_NAME_MAX];
    char curl[PET_CONFIG_CURL_MAX];
    char json_path[PET_CONFIG_JSON_MAX];
    char template_text[PET_CONFIG_TEMPLATE_MAX];
    char value[PET_CONFIG_VALUE_MAX];
} PetConfigItem;

typedef struct {
    char path[4096];
    char sound_path[PET_CONFIG_SOUND_MAX];
    PetConfigItem items[PET_CONFIG_MAX_ITEMS];
    int count;
    int active;
} PetConfig;

void pet_config_init(PetConfig *config, const char *path);
int pet_config_load(PetConfig *config, const char *path);
int pet_config_save(const PetConfig *config);
int pet_config_switch(PetConfig *config);
int pet_config_fetch_text(const PetConfigItem *item, char *output, size_t output_size);
int pet_config_format_value(const PetConfigItem *item, const char *raw_json,
                            char *output, size_t output_size);

#endif
