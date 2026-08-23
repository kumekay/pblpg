#include <pebble.h>
#include "store.h"
#include "list_window.h"
#include "detail_window.h"

static Window *s_list_window;

static void prv_init(void) {
  store_set_list_cb(list_window_refresh);
  store_set_detail_cb(detail_window_refresh);

  s_list_window = list_window_create();
  window_stack_push(s_list_window, true);

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
