#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef signed char        int8;
typedef short              int16;
typedef int                int32;
typedef long long          int64;

typedef unsigned char      uint8;
typedef unsigned short     uint16;
typedef unsigned int       uint32;
typedef unsigned long long uint64;

typedef float    f32;
typedef double   f64;

typedef bool     b8;



#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   (__builtin_expect(!!(x), 1))
    #define UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif



#if defined(_MSC_VER)
    #define DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
    #define DEBUG_BREAK() __builtin_trap()
#else
    #include <signal.h>
    #define DEBUG_BREAK() raise(SIGTRAP)
#endif



#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)



typedef enum log_level
{
    LOG_LEVEL_TRACE,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} log_level;



// Core/Log.c. Writes to stdout (stderr from WARN up), in color only when
// the stream is a terminal, so a log redirected to a file (an exported
// game's log.txt) stays plain text.
void log_message(log_level level, const char* file, int line, const char* fmt, ...);

// Called with a FATAL's message (no trailing newline) after it's logged and
// before the program aborts, e.g. to show it to a player who has no console.
// NULL (the default) for none.
typedef void (*log_fatal_hook)(const char* message);
void log_set_fatal_hook(log_fatal_hook hook);

// Also gets every message, after it's written out: `file`/`line` are "" / 0
// for TRACE and INFO, `message` has no trailing newline (and is cut at 4 KB).
// For showing the log in an app (UI/LogPanel.h). One at a time; NULL for none.
typedef void (*log_sink)(log_level level, const char* file, int line, const char* message, void* user_data);
void log_set_sink(log_sink sink, void* user_data);



#define LOG_TRACE(...) \
    log_message(LOG_LEVEL_TRACE, "", 0, __VA_ARGS__)

#define LOG_INFO(...) \
    log_message(LOG_LEVEL_INFO, "", 0, __VA_ARGS__)

#define LOG_DEBUG(...) \
    log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__ , __VA_ARGS__)

#define LOG_WARN(...) \
    log_message(LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_ERROR(...) \
    log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)



#define FATAL(...)                                      \
    do                                                  \
    {                                                   \
        log_message(LOG_LEVEL_FATAL,                    \
                    __FILE__,                           \
                    __LINE__,                           \
                    __VA_ARGS__);                       \
        abort();                                        \
    } while (0)



#ifndef NDEBUG

#define ASSERT(expr)                                    \
    do                                                  \
    {                                                   \
        if (UNLIKELY(!(expr)))                          \
        {                                               \
            LOG_ERROR("Assertion failed: %s", #expr);   \
            DEBUG_BREAK();                              \
            assert(expr);                               \
        }                                               \
    } while (0)

#else

#define ASSERT(expr) ((void)0)

#endif



#ifndef NDEBUG

#define VERIFY(expr) ASSERT(expr)

#else

#define VERIFY(expr) ((void)(expr))

#endif



#define UNREACHABLE()                                   \
    do                                                  \
    {                                                   \
        FATAL("Unreachable code reached.");             \
    } while (0)



#define TODO(msg)                                       \
    LOG_WARN("TODO: %s", msg)

