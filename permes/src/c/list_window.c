#include "list_window.h"
#include "store.h"
#include "detail_window.h"

static Window *s_window;
static MenuLayer *s_menu;

static uint16_t prv_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return 1 + store_count();
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  if (idx->row == 0) {
    menu_cell_basic_draw(ctx, cell, "+ New thread", NULL, NULL);
    return;
  }
  Thread *th = store_get(idx->row - 1);
  if (!th) return;
  const char *title = th->title[0] ? th->title : th->id;
  const char *subtitle = th->active ? "thinking..." : NULL;
  menu_cell_basic_draw(ctx, cell, title, subtitle, NULL);
}

static void prv_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) {
    store_request_new();
    detail_window_push();  // content fills in when OP_NEW_OK arrives
    return;
  }
  int ti = idx->row - 1;
  Thread *th = store_get(ti);
  if (!th) return;
  store_set_open(ti);
  detail_window_push();
  store_request_open(th->id);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_get_num_rows,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  menu_layer_set_normal_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu, GColorJaegerGreen, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void prv_window_unload(Window *window) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
}

Window *list_window_create(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  return s_window;
}

void list_window_refresh(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
}
