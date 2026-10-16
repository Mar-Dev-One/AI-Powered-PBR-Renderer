#include "FileIO.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <direct.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
#endif

char* file_read_to_string(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        LOG_ERROR("Could not open file: %s\n", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        LOG_ERROR("Could not seek in file: %s\n", path);
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0)
    {
        LOG_ERROR("Could not determine size of file: %s\n", path);
        fclose(file);
        return NULL;
    }

    rewind(file);

    char* buffer = malloc((size_t)size + 1);
    if (!buffer)
        FATAL("Out of memory reading file: %s", path);

    size_t read_count = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    if (read_count != (size_t)size)
    {
        LOG_ERROR("Short read on file: %s\n", path);
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}

static b8
is_dot_entry(const char* name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

b8 file_list_directory(const char* directory, file_visit_fn visit, void* user_data)
{
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s/*", directory);

    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        if (!is_dot_entry(entry.cFileName))
            visit(entry.cFileName, (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user_data);
    } while (FindNextFileA(find, &entry));

    FindClose(find);
    return true;
#else
    DIR* dir = opendir(directory);
    if (!dir)
        return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (is_dot_entry(entry->d_name))
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

        struct stat info;
        b8 is_directory = stat(path, &info) == 0 && S_ISDIR(info.st_mode);
        visit(entry->d_name, is_directory, user_data);
    }

    closedir(dir);
    return true;
#endif
}

b8 file_write_string(const char* path, const char* text)
{
    FILE* file = fopen(path, "wb");
    if (!file)
    {
        LOG_ERROR("Could not open file for writing: %s\n", path);
        return false;
    }

    size_t length  = strlen(text);
    size_t written = fwrite(text, 1, length, file);
    b8     ok      = fclose(file) == 0 && written == length;

    if (!ok)
        LOG_ERROR("Could not write file: %s\n", path);
    return ok;
}

b8 file_make_directory(const char* path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0)
        return true;
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    if (mkdir(path, 0755) == 0)
        return true;
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

b8 file_exists(const char* path)
{
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
#endif
}

b8 file_is_directory(const char* path)
{
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

b8 file_make_directories(const char* path)
{
    char partial[1024];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(partial))
        return false;
    memcpy(partial, path, length + 1);

    // Each prefix ending at a separator, then the whole path. Prefixes that
    // can't be made (a drive "C:", or a folder that already exists) are fine:
    // only the final check counts.
    for (size_t i = 1; i < length; ++i)
    {
        if (partial[i] != '/' && partial[i] != '\\')
            continue;
        char separator = partial[i];
        partial[i] = '\0';
        if (!file_is_directory(partial))
            file_make_directory(partial);
        partial[i] = separator;
    }
    return file_make_directory(partial);
}

b8 file_copy(const char* source, const char* destination)
{
    FILE* in = fopen(source, "rb");
    if (!in)
    {
        LOG_ERROR("Could not open file: %s\n", source);
        return false;
    }

    FILE* out = fopen(destination, "wb");
    if (!out)
    {
        LOG_ERROR("Could not open file for writing: %s\n", destination);
        fclose(in);
        return false;
    }

    char   buffer[64 * 1024];
    size_t count;
    b8     ok = true;
    while (ok && (count = fread(buffer, 1, sizeof(buffer), in)) > 0)
        ok = fwrite(buffer, 1, count, out) == count;

    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;

    if (!ok)
        LOG_ERROR("Could not copy %s to %s\n", source, destination);
    return ok;
}
