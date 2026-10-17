#include "Dialog.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

#ifdef _WIN32
static wchar_t*
widen(const char* text)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t* wide = length > 0 ? malloc((size_t)length * sizeof(wchar_t)) : NULL;
    if (wide)
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
    return wide;
}
#endif

void dialog_show_error(const char* title, const char* message)
{
#ifdef _WIN32
    wchar_t* wide_title   = widen(title);
    wchar_t* wide_message = widen(message);

    // Topmost: the game's own (possibly fullscreen) window would hide it.
    if (wide_title && wide_message)
        MessageBoxW(NULL, wide_message, wide_title, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    else
        MessageBoxA(NULL, message, title, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);

    free(wide_title);
    free(wide_message);
#else
    fprintf(stderr, "%s: %s\n", title, message);
#endif
}
