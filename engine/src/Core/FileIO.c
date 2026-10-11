#include "FileIO.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
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
