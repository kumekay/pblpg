#include "list_window.h"
#include "store.h"
#include "detail_window.h"

static Window *s_window;
static MenuLayer *s_menu;
static TextLayer *s_error_title;
static TextLayer *s_error_body;
static bool s_has_appeared;

static void prv_update_error(void) {
  const char *error = store_error();
  bool visible = error && error[0];
  if (s_menu) layer_set_hidden(menu_layer_get_layer(s_menu), visible);
  if (s_error_title) layer_set_hidden(text_layer_get_layer(s_error_title), !visible);
  if (s_error_body) {
    text_layer_set_text(s_error_body, visible ? error : "");
    layer_set_hidden(text_layer_get_layer(s_error_body), !visible);
  }
}

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
  if (store_error()[0]) return;
  if (idx->row == 0) {
    store_begin_new();
    detail_window_push();  // remote session is created after the first message
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

  s_error_title = text_layer_create(GRect(12, 32, bounds.size.w - 24, 34));
  text_layer_set_background_color(s_error_title, GColorWhite);
  text_layer_set_text_color(s_error_title, GColorBlack);
  text_layer_set_font(s_error_title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_error_title, GTextAlignmentCenter);
  text_layer_set_text(s_error_title, "Setup required");
  layer_add_child(root, text_layer_get_layer(s_error_title));

  s_error_body = text_layer_create(GRect(12, 70, bounds.size.w - 24, 110));
  text_layer_set_background_color(s_error_body, GColorWhite);
  text_layer_set_text_color(s_error_body, GColorBlack);
  text_layer_set_font(s_error_body, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_alignment(s_error_body, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_error_body));
  prv_update_error();
}

static void prv_window_appear(Window *window) {
  // Reconcile with the server whenever the user returns from a thread. This
  // picks up newly-created sessions and any title/order changes made while the
  // detail window was open. The initial list is requested by store_init().
  if (s_has_appeared) store_request_list();
  s_has_appeared = true;
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_error_body);
  text_layer_destroy(s_error_title);
  menu_layer_destroy(s_menu);
  s_error_body = NULL;
  s_error_title = NULL;
  s_menu = NULL;
}

Window *list_window_create(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .appear = prv_window_appear,
    .unload = prv_window_unload,
  });
  return s_window;
}

void list_window_refresh(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
  prv_update_error();
}
