#include "common/log.h"
#include <stdarg.h>
#include <pthread.h>
#include <execinfo.h>
#include <unistd.h>

#define BACKTRACE_SIZE 32

void hfsss_panic(const char *file, int line, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "HFSSS PANIC\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "File: %s\n", file);
    fprintf(stderr, "Line: %d\n", line);
    fprintf(stderr, "Message: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n\n");

    /* Print backtrace */
    void *buffer[BACKTRACE_SIZE];
    int nptrs = backtrace(buffer, BACKTRACE_SIZE);
    fprintf(stderr, "Backtrace:\n");
    backtrace_symbols_fd(buffer, nptrs, STDERR_FILENO);
    fprintf(stderr, "\n========================================\n");

    /* Abort */
    abort();
}

struct log_lock {
    pthread_mutex_t mutex;
};

int log_init(struct log_ctx *ctx, u32 buffer_size, u32 level)
{
    if (!ctx || buffer_size == 0) {
        /* A zero-size ring buffer is not valid: log_printf() would later
         * divide by buffer_size to compute head/tail, producing UB. Reject
         * it at init time instead of building a broken context. */
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->buffer = (struct log_entry *)calloc(buffer_size, sizeof(struct log_entry));
    if (!ctx->buffer) {
        return HFSSS_ERR_NOMEM;
    }

    ctx->buffer_size = buffer_size;
    ctx->level = level;
    ctx->head = 0;
    ctx->tail = 0;
    ctx->count = 0;
    ctx->use_stdout = 1;

    struct log_lock *lock = (struct log_lock *)malloc(sizeof(struct log_lock));
    if (!lock) {
        free(ctx->buffer);
        return HFSSS_ERR_NOMEM;
    }
    pthread_mutex_init(&lock->mutex, NULL);
    ctx->lock = lock;

    return HFSSS_OK;
}

void log_cleanup(struct log_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->lock) {
        struct log_lock *lock = (struct log_lock *)ctx->lock;
        pthread_mutex_destroy(&lock->mutex);
        free(lock);
    }

    if (ctx->buffer) {
        free(ctx->buffer);
    }

    if (ctx->output_file) {
        fclose(ctx->output_file);
    }

    memset(ctx, 0, sizeof(*ctx));
}

void log_set_output_file(struct log_ctx *ctx, const char *filename)
{
    if (!ctx) {
        return;
    }

    if (ctx->output_file) {
        fclose(ctx->output_file);
    }

    if (filename) {
        ctx->output_file = fopen(filename, "a");
    } else {
        ctx->output_file = NULL;
    }
}

void log_set_level(struct log_ctx *ctx, u32 level)
{
    if (!ctx) {
        return;
    }
    ctx->level = level;
}

void log_printf(struct log_ctx *ctx, u32 level, const char *module, const char *file, u32 line, const char *fmt, ...)
{
    if (!ctx || level > ctx->level) {
        return;
    }

    struct log_lock *lock = (struct log_lock *)ctx->lock;
    pthread_mutex_lock(&lock->mutex);

    struct log_entry *entry = &ctx->buffer[ctx->head];
    entry->timestamp = get_time_ns();
    entry->level = level;
    entry->module = module;
    entry->file = file;
    entry->line = line;

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, LOG_ENTRY_SIZE, fmt, args);
    va_end(args);

    if (ctx->use_stdout) {
        const char *level_str[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
        printf("[%s] [%s] %s:%u: %s\n", level_str[MIN(level, 4)], module, file, line, entry->message);
    }

    if (ctx->output_file) {
        const char *level_str[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
        fprintf(ctx->output_file, "[%llu] [%s] [%s] %s:%u: %s\n", (unsigned long long)entry->timestamp,
                level_str[MIN(level, 4)], module, file, line, entry->message);
        fflush(ctx->output_file);
    }

    ctx->head = (ctx->head + 1) % ctx->buffer_size;
    if (ctx->count < ctx->buffer_size) {
        ctx->count++;
    } else {
        ctx->tail = (ctx->tail + 1) % ctx->buffer_size;
    }

    pthread_mutex_unlock(&lock->mutex);
}
