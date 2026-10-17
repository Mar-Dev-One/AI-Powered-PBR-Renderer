#include "Defines.h"

#include <string.h>

#ifdef _WIN32
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
#else
    #include <unistd.h>
#endif

static log_fatal_hook fatal_hook = NULL;
static log_sink       sink       = NULL;
static void*          sink_user  = NULL;

void log_set_fatal_hook(log_fatal_hook hook)
{
    fatal_hook = hook;
}

void log_set_sink(log_sink new_sink, void* user_data)
{
    sink      = new_sink;
    sink_user = user_data;
}

void log_message(log_level level, const char* file, int line, const char* fmt, ...)
{
    static const char* names[] =
    {
        "TRACE",
        "DEBUG",
        "INFO",
        "WARN",
        "ERROR",
        "FATAL"
    };

    static const char* colors[] =
    {
        "\033[90m",
        "\033[36m",
        "\033[32m",
        "\033[33m",
        "\033[31m",
        "\033[35m"
    };

    FILE* stream =
        (level >= LOG_LEVEL_WARN)
        ? stderr
        : stdout;

    // Only a terminal understands the color codes; in a file they're noise.
    b8 color = isatty(fileno(stream)) != 0;

    if (level == LOG_LEVEL_TRACE || level == LOG_LEVEL_INFO) {
        fprintf(stream,
            "%s[%s] : ",
            color ? colors[level] : "",
            names[level]
        );
    }else {
        fprintf(stream,
            "%s[%s] %s:%d: ",
            color ? colors[level] : "",
            names[level],
            file,
            line);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);

    if (color)
        fprintf(stream, "\033[0m");

    if (level == LOG_LEVEL_FATAL)
    {
        // FATAL's messages carry no newline of their own; end the line so
        // the file doesn't stop mid-line.
        fprintf(stream, "\n");
        fflush(stream);
    }

    if (!sink && !(level == LOG_LEVEL_FATAL && fatal_hook))
        return;

    // The message once more, for the sink and the fatal hook: without the
    // trailing newline most messages end with. Long ones (a shader's whole
    // error log) are cut at the buffer's size.
    char message[4096];
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    size_t length = strlen(message);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        message[--length] = '\0';

    if (sink)
        sink(level, file, line, message, sink_user);
    if (level == LOG_LEVEL_FATAL && fatal_hook)
        fatal_hook(message);
}
