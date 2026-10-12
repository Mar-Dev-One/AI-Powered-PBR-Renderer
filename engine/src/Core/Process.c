#include "Process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <signal.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

struct process
{
#ifdef _WIN32
    HANDLE handle;
#else
    pid_t  pid;
#endif
    b8     exited;
    int    exit_code;
};

#ifdef _WIN32

// Appends one argument to a Windows command line, quoted the way
// CommandLineToArgvW (and so the C runtime) parses it back.
static void
append_argument(char* line, size_t size, const char* arg)
{
    size_t used = strlen(line);
    if (used && used + 1 < size)
        line[used++] = ' ';

    b8 quote = !arg[0] || strpbrk(arg, " \t\"") != NULL;
    if (quote && used + 1 < size)
        line[used++] = '"';

    size_t backslashes = 0;
    for (const char* c = arg; *c && used + 2 < size; ++c)
    {
        if (*c == '\\')
        {
            ++backslashes;
        }
        else
        {
            // Backslashes only escape when they precede a quote.
            if (*c == '"')
            {
                for (size_t i = 0; i < backslashes + 1 && used + 1 < size; ++i)
                    line[used++] = '\\';
            }
            backslashes = 0;
        }
        line[used++] = *c;
    }
    if (quote)
    {
        for (size_t i = 0; i < backslashes && used + 1 < size; ++i)
            line[used++] = '\\';
        if (used + 1 < size)
            line[used++] = '"';
    }
    line[used] = '\0';
}

process*
process_spawn(const char* const* argv, const char* log_path)
{
    char line[4096] = "";
    for (const char* const* a = argv; *a; ++a)
        append_argument(line, sizeof(line), *a);

    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE log = log_path
        ? CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
        : INVALID_HANDLE_VALUE;

    STARTUPINFOEXA startup;
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);

    // The child inherits the log file and nothing else -- in particular not
    // the command server's sockets, which would otherwise outlive the app.
    char   attribute_storage[256];
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    if (log != INVALID_HANDLE_VALUE)
    {
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
        if (attribute_size <= sizeof(attribute_storage))
        {
            attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)attribute_storage;
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)
                || !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(log), NULL, NULL))
                attributes = NULL;
        }

        startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput  = NULL;
        startup.StartupInfo.hStdOutput = log;
        startup.StartupInfo.hStdError  = log;
        startup.lpAttributeList        = attributes;
    }

    PROCESS_INFORMATION info;
    BOOL started = CreateProcessA(argv[0], line, NULL, NULL, attributes != NULL,
                                  CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                  &startup.StartupInfo, &info);
    if (attributes)
        DeleteProcThreadAttributeList(attributes);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);   // the child has its own copy

    if (!started)
    {
        LOG_ERROR("process: could not start '%s' (error %lu)\n", argv[0], GetLastError());
        return NULL;
    }
    CloseHandle(info.hThread);

    process* p = calloc(1, sizeof(*p));
    if (!p)
        FATAL("process: out of memory");
    p->handle = info.hProcess;
    return p;
}

b8
process_running(process* p, int* out_exit_code)
{
    if (!p->exited && WaitForSingleObject(p->handle, 0) == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(p->handle, &code);
        p->exited    = true;
        p->exit_code = (int)code;
    }
    if (p->exited && out_exit_code)
        *out_exit_code = p->exit_code;
    return !p->exited;
}

void
process_destroy(process* p)
{
    if (!p)
        return;
    if (process_running(p, NULL))
    {
        TerminateProcess(p->handle, 1);
        WaitForSingleObject(p->handle, 2000);
    }
    CloseHandle(p->handle);
    free(p);
}

#else

process*
process_spawn(const char* const* argv, const char* log_path)
{
    // Opened before forking, so a bad path is reported here.
    int log = log_path ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : open("/dev/null", O_WRONLY);

    pid_t pid = fork();
    if (pid < 0)
    {
        LOG_ERROR("process: fork failed\n");
        if (log >= 0)
            close(log);
        return NULL;
    }

    if (pid == 0)
    {
        if (log >= 0)
        {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        int null_in = open("/dev/null", O_RDONLY);
        if (null_in >= 0)
            dup2(null_in, STDIN_FILENO);

        // Nothing else of ours -- in particular not the command server's
        // sockets, which would otherwise outlive the app.
        long max_fd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < (max_fd > 0 && max_fd < 4096 ? max_fd : 4096); ++fd)
            close(fd);

        execv(argv[0], (char* const*)argv);
        fprintf(stderr, "process: could not run '%s' (errno %d)\n", argv[0], errno);
        _exit(127);
    }

    if (log >= 0)
        close(log);

    process* p = calloc(1, sizeof(*p));
    if (!p)
        FATAL("process: out of memory");
    p->pid = pid;
    return p;
}

b8
process_running(process* p, int* out_exit_code)
{
    if (!p->exited)
    {
        int status;
        if (waitpid(p->pid, &status, WNOHANG) == p->pid)
        {
            p->exited    = true;
            p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
    }
    if (p->exited && out_exit_code)
        *out_exit_code = p->exit_code;
    return !p->exited;
}

void
process_destroy(process* p)
{
    if (!p)
        return;
    if (process_running(p, NULL))
    {
        kill(p->pid, SIGTERM);
        waitpid(p->pid, NULL, 0);
    }
    free(p);
}

#endif
