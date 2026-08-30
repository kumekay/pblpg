#pragma once

#include <pebble.h>

// Thread detail screen: title, scrollable reply, SELECT = speak (dictation).
void detail_window_push(void);
void detail_window_refresh(void);
void detail_window_notify_reply_complete(void);
