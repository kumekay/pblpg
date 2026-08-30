#pragma once

#include <pebble.h>

// Thread detail screen: title, scrollable reply, SELECT = speak (dictation).
// When dictate_immediately is true, dictation starts as soon as the screen appears.
void detail_window_push(bool dictate_immediately);
void detail_window_refresh(void);
void detail_window_notify_reply_complete(void);
