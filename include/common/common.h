#ifndef __HFSSS_COMMON_H
#define __HFSSS_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

/* Common Types */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* Common Macros */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* Maximum values */
#define U64_MAX (0xFFFFFFFFFFFFFFFFULL)
#define U32_MAX (0xFFFFFFFFUL)
#define U16_MAX (0xFFFF)
#define U8_MAX  (0xFF)
#define container_of(ptr, type, member) ({ \
    const typeof(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offsetof(type, member)); \
})

/* Error Codes */
#define HFSSS_OK           0
#define HFSSS_ERR          (-1)
#define HFSSS_ERR_INVAL    (-2)
#define HFSSS_ERR_NOMEM    (-3)
#define HFSSS_ERR_BUSY     (-4)
#define HFSSS_ERR_TIMEOUT  (-5)
#define HFSSS_ERR_NOENT    (-6)
#define HFSSS_ERR_EXIST    (-7)
#define HFSSS_ERR_NOSPC    (-8)
#define HFSSS_ERR_NOTSUPP  (-9)
#define HFSSS_ERR_AGAIN    (-10)
#define HFSSS_ERR_IO       (-100)

/* Enterprise error codes */
#define HFSSS_ERR_PI_GUARD   (-101)  /* T10 PI guard tag CRC mismatch */
#define HFSSS_ERR_PI_REFTAG  (-102)  /* T10 PI reference tag mismatch */
#define HFSSS_ERR_PI_APPTAG  (-103)  /* T10 PI application tag mismatch */
#define HFSSS_ERR_CRYPTO     (-104)  /* Encryption/decryption failure */
#define HFSSS_ERR_AUTH       (-105)  /* Authentication failure */
#define HFSSS_ERR_LOCKED     (-106)  /* Resource locked (TCG Opal) */
#define HFSSS_ERR_POWER_LOSS (-107)  /* Power loss detected */
#define HFSSS_ERR_THERMAL    (-108)  /* Thermal shutdown triggered */

/* CRC-16 (T10 DIF polynomial) */
u16 t10_crc16(const void *data, size_t len);

/* Log Levels */
#define LOG_LEVEL_ERROR    0
#define LOG_LEVEL_WARN     1
#define LOG_LEVEL_INFO     2
#define LOG_LEVEL_DEBUG    3
#define LOG_LEVEL_TRACE    4

/* Time Helpers */
static inline u64 get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static inline void sleep_ns(u64 ns) {
    struct timespec ts;
    ts.tv_sec = ns / 1000000000ULL;
    ts.tv_nsec = ns % 1000000000ULL;
    nanosleep(&ts, NULL);
}

/* CRC-32 (IEEE 802.3) */
u32 hfsss_crc32(const void *data, size_t len);

/* Panic and Assert Handling */
void hfsss_panic(const char *file, int line, const char *fmt, ...);

#define HFSSS_PANIC(...) hfsss_panic(__FILE__, __LINE__, __VA_ARGS__)

#ifdef NDEBUG
#define HFSSS_ASSERT(cond) ((void)0)
#define HFSSS_ASSERT_MSG(cond, msg) ((void)0)
#else
#define HFSSS_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            HFSSS_PANIC("Assertion failed: %s", #cond); \
        } \
    } while(0)
#define HFSSS_ASSERT_MSG(cond, msg) \
    do { \
        if (!(cond)) { \
            HFSSS_PANIC("Assertion failed: %s - %s", #cond, msg); \
        } \
    } while(0)
#endif

#endif /* __HFSSS_COMMON_H */
