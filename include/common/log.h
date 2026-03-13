#ifndef __HFSSS_LOG_H
#define __HFSSS_LOG_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BUFFER_SIZE 1024
#define LOG_ENTRY_SIZE  512

/* Log Entry */
struct log_entry {
    u64 timestamp;
    u32 level;
    const char *module;
    const char *file;
    u32 line;
    char message[LOG_ENTRY_SIZE];
};

/* Log Context */
struct log_ctx {
    struct log_entry *buffer;
    u32 buffer_size;
    u32 head;
    u32 tail;
    u32 count;
    u32 level;
    FILE *output_file;
    int use_stdout;
    void *lock;
};

/* Function Prototypes */
int log_init(struct log_ctx *ctx, u32 buffer_size, u32 level);
void log_cleanup(struct log_ctx *ctx);
void log_set_output_file(struct log_ctx *ctx, const char *filename);
void log_set_level(struct log_ctx *ctx, u32 level);
void log_printf(struct log_ctx *ctx, u32 level, const char *module,
                const char *file, u32 line, const char *fmt, ...);

/* Convenience Macros */
#define log_error(ctx, module, ...) \
    log_printf(ctx, LOG_LEVEL_ERROR, module, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(ctx, module, ...) \
    log_printf(ctx, LOG_LEVEL_WARN, module, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(ctx, module, ...) \
    log_printf(ctx, LOG_LEVEL_INFO, module, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(ctx, module, ...) \
    log_printf(ctx, LOG_LEVEL_DEBUG, module, __FILE__, __LINE__, __VA_ARGS__)
#define log_trace(ctx, module, ...) \
    log_printf(ctx, LOG_LEVEL_TRACE, module, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_LOG_H */
