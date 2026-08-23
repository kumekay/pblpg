#pragma once

#include <pebble.h>

// Thread list screen. Row 0 = "New thread", then one row per hermes session.
Window *list_window_create(void);
void list_window_refresh(void);
