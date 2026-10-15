#include "list_window.h"
#include "store.h"
#include "detail_window.h"

static Window *s_window;
static MenuLayer *s_menu;
static TextLayer *s_error_title;
static TextLayer *s_error_body;
static bool s_has_appeared;

// Thread rows: up to two lines of bold title, then a small line with the time
// of the last message (or "thinking..." while a run is in progress).
#define ROW_PAD_X     PBL_IF_ROUND_ELSE(16, 6)
#define ROW_TITLE_MAX 44  // two lines of GOTHIC_18_BOLD
#define ROW_META_H    18
#define ROW_PAD_B     6
#define ROW_BASIC_H   44  // default MenuLayer cell height
#define ROW_ALIGN     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft)

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

static const char *prv_title(Thread *th) {
  return th->title[0] ? th->title : th->id;
}

// Short "when" label: "now", "5m ago", "14:32", "Yesterday", "Monday", "12 Oct".
static void prv_format_time(int32_t when, char *buf, size_t size) {
  buf[0] = '\0';
  if (when <= 0) return;
  time_t now = time(NULL);
  time_t t = when;
  int32_t age = now - t;
  if (age < 60) {
    snprintf(buf, size, "now");
    return;
  }
  if (age < 3600) {
    snprintf(buf, size, "%dm ago", (int)(age / 60));
    return;
  }
  struct tm today = *localtime(&now);
  struct tm then = *localtime(&t);
  int days = (today.tm_year - then.tm_year) * 365 + today.tm_yday - then.tm_yday;
  if (days <= 0) {
    strftime(buf, size, clock_is_24h_style() ? "%H:%M" : "%I:%M %p", &then);
    if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
  } else if (days == 1) {
    snprintf(buf, size, "Yesterday");
  } else if (days < 7) {
    strftime(buf, size, "%A", &then);
  } else {
    strftime(buf, size, "%d %b", &then);
    if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
  }
}

static int16_t prv_title_height(const char *title, int16_t width) {
  GSize size = graphics_text_layout_get_content_size(
      title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(0, 0, width, ROW_TITLE_MAX), GTextOverflowModeTrailingEllipsis, ROW_ALIGN);
  return size.h;
}

static int16_t prv_get_cell_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) return ROW_BASIC_H;
  Thread *th = store_get(idx->row - 1);
  if (!th) return ROW_BASIC_H;
  int16_t width = layer_get_bounds(menu_layer_get_layer(menu)).size.w - 2 * ROW_PAD_X;
  bool has_meta = th->active || th->last_active > 0;
  return prv_title_height(prv_title(th), width) + (has_meta ? ROW_META_H : 0) + ROW_PAD_B;
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  if (idx->row == 0) {
    menu_cell_basic_draw(ctx, cell, "+ New thread", NULL, NULL);
    return;
  }
  Thread *th = store_get(idx->row - 1);
  if (!th) return;
  GRect bounds = layer_get_bounds(cell);
  int16_t width = bounds.size.w - 2 * ROW_PAD_X;
  const char *title = prv_title(th);
  int16_t title_h = prv_title_height(title, width);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(ROW_PAD_X, 0, width, title_h),
                     GTextOverflowModeTrailingEllipsis, ROW_ALIGN, NULL);

  char when[16];
  const char *meta = when;
  if (th->active) {
    meta = "thinking...";
  } else {
    prv_format_time(th->last_active, when, sizeof(when));
  }
  if (!meta[0]) return;
  if (!menu_cell_layer_is_highlighted(cell)) {
    graphics_context_set_text_color(
        ctx, th->active ? PBL_IF_COLOR_ELSE(GColorJaegerGreen, GColorBlack)
                        : PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  }
  graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(ROW_PAD_X, title_h, width, ROW_META_H),
                     GTextOverflowModeTrailingEllipsis, ROW_ALIGN, NULL);
}

static void prv_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (store_error()[0]) return;
  if (idx->row == 0) {
    store_begin_new();
    detail_window_push(true);  // record now; create the remote session on send
    return;
  }
  int ti = idx->row - 1;
  Thread *th = store_get(ti);
  if (!th) return;
  store_set_open(ti);
  detail_window_push(false);
  store_request_open(th->id);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_get_num_rows,
    .get_cell_height = prv_get_cell_height,
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

  // Height is bounds-relative so the error text fits on shorter displays
  // (basalt 200x168, chalk 180x180) as well as emery's 200x228.
  s_error_body = text_layer_create(
      GRect(12, 70, bounds.size.w - 24, bounds.size.h - 70 - 8));
  text_layer_set_background_color(s_error_body, GColorWhite);
  text_layer_set_text_color(s_error_body, GColorBlack);
  text_layer_set_font(s_error_body, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_alignment(s_error_body, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_error_body));
  prv_update_error();
}

static void prv_minute_tick(struct tm *tick_time, TimeUnits changed) {
  // Keep relative times ("5m ago") current while the list is visible.
  if (s_menu) layer_mark_dirty(menu_layer_get_layer(s_menu));
}

static void prv_window_appear(Window *window) {
  tick_timer_service_subscribe(MINUTE_UNIT, prv_minute_tick);
  // Reconcile with the server whenever the user returns from a thread. This
  // picks up newly-created sessions and any title/order changes made while the
  // detail window was open. The initial list is requested by store_init().
  if (s_has_appeared) store_request_list();
  s_has_appeared = true;
}

static void prv_window_disappear(Window *window) {
  tick_timer_service_unsubscribe();
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
    .disappear = prv_window_disappear,
    .unload = prv_window_unload,
  });
  return s_window;
}

void list_window_refresh(void) {
  if (s_menu) menu_layer_reload_data(s_menu);
  prv_update_error();
}
