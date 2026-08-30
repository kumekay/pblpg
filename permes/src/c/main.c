#include <pebble.h>
#include "store.h"
#include "list_window.h"
#include "detail_window.h"

static Window *s_list_window;

static void prv_init(void) {
  store_set_list_cb(list_window_refresh);
  store_set_detail_cb(detail_window_refresh);
  store_set_completion_cb(detail_window_notify_reply_complete);

  s_list_window = list_window_create();
  window_stack_push(s_list_window, false);

  // The default landing screen is a fresh local draft. Keeping the thread
  // list underneath means BACK from dictation returns directly to the list.
  store_begin_new();
  detail_window_push(true);

  store_init();  // opens AppMessage + requests the thread list
}

static void prv_deinit(void) {
  store_deinit();
  window_destroy(s_list_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
