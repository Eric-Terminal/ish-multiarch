#include <stddef.h>
#include <stdbool.h>

struct user_default_key {
    char *name;
    char *underlying_name;
};

extern char **(*get_all_defaults_keys)(void);
extern char *(*get_friendly_name)(const char *name);
extern char *(*get_underlying_name)(const char *name);
extern bool (*get_user_default)(const char *name, char **buffer, size_t *size);
extern bool (*set_user_default)(const char *name, char *buffer, size_t size);
extern bool (*remove_user_default)(const char *name);
extern char *(*get_documents_directory)(void);

// 宿主必须在发布 guest 前一次性提供完整的 defaults 回调组。
void ish_install_user_defaults_callbacks(
        char **(*all_keys)(void),
        char *(*friendly_name)(const char *name),
        char *(*underlying_name)(const char *name),
        bool (*read_value)(
                const char *name, char **buffer, size_t *size),
        bool (*write_value)(
                const char *name, char *buffer, size_t size),
        bool (*remove_value)(const char *name));
