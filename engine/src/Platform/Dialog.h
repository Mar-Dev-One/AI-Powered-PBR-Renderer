#pragma once

#include "../Core/Defines.h"

// A modal error box the user dismisses with OK, for programs with no console
// (an exported game) to say why they're closing. `title` and `message` are
// UTF-8. Blocks until dismissed. Elsewhere than Windows it only writes to
// stderr.
void dialog_show_error(const char* title, const char* message);
