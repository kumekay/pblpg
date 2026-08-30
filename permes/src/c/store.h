#pragma once

#include <pebble.h>

#define MAX_THREADS      20
#define THREAD_ID_LEN    48
#define THREAD_TITLE_LEN 128
#define REPLY_MAX        14000
#define REPLY_CHUNK_MAX  32

typedef struct {
  char id[THREAD_ID_LEN];
  char title[THREAD_TITLE_LEN];
  bool active;  // run in progress on this thread
  bool used;
} Thread;

// Lifecycle
void store_init(void);
void store_deinit(void);

// Thread list access
int store_count(void);
const char *store_error(void); // latest phone-side error, or an empty string
Thread *store_get(int index);
int store_find(const char *id);  // returns index or -1
Thread *store_open_thread(void); // currently open thread or NULL
void store_set_open(int index);  // -1 = none

// Reply state for the open thread
const char *store_reply(void);     // assembled reply (may be empty)
bool store_reply_pending(void);    // chunks arriving right now
void store_mark_failed(const char *error);
void store_append_you(const char *text);  // locally echo the dictated message

// Scroll hints for the detail window (consumed on read)
#define SCROLL_HINT_NONE 0
#define SCROLL_HINT_TOP 1
#define SCROLL_HINT_BOTTOM 2
int store_scroll_hint(void);

// Requests to the phone side
void store_request_list(void);
void store_request_new(void);
void store_request_open(const char *id);
void store_send_message(const char *id, const char *text);

// UI hooks
typedef void (*StoreRefreshCb)(void);
typedef void (*StoreCompletionCb)(void);
void store_set_list_cb(StoreRefreshCb cb);
void store_set_detail_cb(StoreRefreshCb cb);
void store_set_completion_cb(StoreCompletionCb cb);
