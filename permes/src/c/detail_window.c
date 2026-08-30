#include "detail_window.h"
#include "store.h"

#define DICTATION_BUF_SIZE 512
#define SCROLL_STEP 44
#define EXTRA_LAYERS 2                       // thinking indicator + footer
#define MAX_MSG_LAYERS (MSG_MAX + EXTRA_LAYERS)

// One transcript message rendered as a custom layer: background bubble
// (optional) + text drawn with insets.
typedef struct {
  const char *text;
  GFont font;
  GColor bg;
  GColor fg;
  GEdgeInsets insets;
} MsgLayerData;

static Window *s_window;
static TextLayer *s_title_layer;
static ScrollLayer *s_scroll;
static Layer *s_msg_layers[MAX_MSG_LAYERS];
static int s_num_layers = 0;
static DictationSession *s_dictation;
static int s_scroll_offset = 0;   // >= 0: pixels scrolled down
static bool s_dictating = false;
static bool s_dictate_on_appear = false;
static bool s_leave_on_dictation_failure = false;

static void prv_msg_update_proc(Layer *layer, GContext *ctx) {
  MsgLayerData *d = (MsgLayerData *)layer_get_data(layer);
  GRect bounds = layer_get_bounds(layer);
  if (!gcolor_equal(d->bg, GColorClear)) {
    graphics_context_set_fill_color(ctx, d->bg);
    graphics_fill_rect(ctx, bounds, 6, GCornersAll);
  }
  graphics_context_set_text_color(ctx, d->fg);
  GRect box = grect_inset(bounds, d->insets);
  graphics_draw_text(ctx, d->text, d->font, box,
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
}

static void prv_clear_layers(void) {
  for (int i = 0; i < s_num_layers; i++) {
    layer_remove_from_parent(s_msg_layers[i]);
    layer_destroy(s_msg_layers[i]);
    s_msg_layers[i] = NULL;
  }
  s_num_layers = 0;
}

// Returns the layer height.
static int prv_add_msg(const char *text, GFont font, GColor bg, GColor fg,
                       GEdgeInsets insets, int y, int w) {
  if (s_num_layers >= MAX_MSG_LAYERS) return 0;
  int tw = w - insets.left - insets.right;
  GSize ts = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, tw, 10000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int h = ts.h + insets.top + insets.bottom;
  Layer *l = layer_create_with_data(GRect(0, y, w, h), sizeof(MsgLayerData));
  MsgLayerData *d = (MsgLayerData *)layer_get_data(l);
  d->text = text;
  d->font = font;
  d->bg = bg;
  d->fg = fg;
  d->insets = insets;
  layer_set_update_proc(l, prv_msg_update_proc);
  scroll_layer_add_child(s_scroll, l);
  s_msg_layers[s_num_layers++] = l;
  return h;
}

static void prv_render(void) {
  Thread *th = store_open_thread();
  if (!th || !s_window || !s_title_layer || !s_scroll) return;

  text_layer_set_text(s_title_layer, th->title[0] ? th->title : th->id);

  prv_clear_layers();

  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll));
  int content_w = frame.size.w;
  int y = 0;

  int n = store_msg_count();
  bool busy = store_reply_pending() || th->active;

  if (n == 0 && !busy) {
    prv_add_msg("Press SELECT and speak to the agent.",
                fonts_get_system_font(FONT_KEY_GOTHIC_24),
                GColorClear, GColorBlack,
                (GEdgeInsets){0, 4, 0, 4}, y, content_w);
  }

  for (int i = 0; i < n; i++) {
    int role;
    const char *text = store_msg(i, &role);
    if (role == MSG_ROLE_USER) {
      // User prompts get a shaded bubble so they stand out from replies
      y += prv_add_msg(text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GColorLightGray, GColorBlack,
                       (GEdgeInsets){8, 12, 8, 12}, y, content_w);
    } else {
      GColor bg = (role == MSG_ROLE_SYSTEM) ? GColorMelon : GColorClear;
      y += prv_add_msg(text, fonts_get_system_font(FONT_KEY_GOTHIC_24),
                       bg, GColorBlack,
                       (GEdgeInsets){2, 4, 8, 4}, y, content_w);
    }
    y += 4;  // gap between messages
  }

  if (busy) {
    y += prv_add_msg("thinking\u2026",
                     fonts_get_system_font(FONT_KEY_GOTHIC_24),
                     GColorClear, GColorDarkGray,
                     (GEdgeInsets){2, 4, 8, 4}, y, content_w);
  } else if (n > 0) {
    y += prv_add_msg("SELECT: speak",
                     fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GColorClear, GColorDarkGray,
                     (GEdgeInsets){2, 4, 4, 4}, y, content_w);
  }

  int16_t content_h = y + 4;
  if (content_h < frame.size.h) content_h = frame.size.h;
  scroll_layer_set_content_size(s_scroll, GSize(content_w, content_h));

  // Scroll position: honour hints, keep in bounds
  int hint = store_scroll_hint();
  int max_offset = content_h - frame.size.h;
  if (hint == SCROLL_HINT_TOP) s_scroll_offset = 0;
  else if (hint == SCROLL_HINT_BOTTOM) s_scroll_offset = max_offset;
  if (s_scroll_offset > max_offset) s_scroll_offset = max_offset;
  if (s_scroll_offset < 0) s_scroll_offset = 0;
  scroll_layer_set_content_offset(s_scroll, GPoint(0, -s_scroll_offset), false);
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

static void prv_scroll_by(int delta) {
  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll));
  GSize content = scroll_layer_get_content_size(s_scroll);
  int max_offset = content.h - frame.size.h;
  if (max_offset <= 0) return;
  s_scroll_offset += delta;
  if (s_scroll_offset < 0) s_scroll_offset = 0;
  if (s_scroll_offset > max_offset) s_scroll_offset = max_offset;
  scroll_layer_set_content_offset(s_scroll, GPoint(0, -s_scroll_offset), true);
}

// ---------------------------------------------------------------------------
// Dictation
// ---------------------------------------------------------------------------

static void prv_send_dictated(char *transcription) {
  Thread *th = store_open_thread();
  if (!th) return;
  store_append_you(transcription);
  store_send_message(th->id, transcription);
  prv_render();
}

static void prv_dictation_cb(DictationSession *session, DictationSessionStatus status,
                             char *transcription, void *ctx) {
  s_dictating = false;
  // The native dictation UI reports BACK through several failure statuses
  // depending on whether listening/transcription had already started.
  bool leave = s_leave_on_dictation_failure &&
               status != DictationSessionStatusSuccess;
  s_leave_on_dictation_failure = false;
  if (status == DictationSessionStatusSuccess && transcription && transcription[0]) {
    prv_send_dictated(transcription);
  } else if (leave && window_stack_get_top_window() == s_window) {
    window_stack_pop(true);
  }
}

// ---------------------------------------------------------------------------
// Clicks
// ---------------------------------------------------------------------------

static void prv_start_dictation(bool leave_on_failure) {
  if (s_dictating) return;
  Thread *th = store_open_thread();
  if (!th || (!th->id[0] && th->active)) return;
  s_dictating = true;
  s_leave_on_dictation_failure = leave_on_failure;
  dictation_session_start(s_dictation);
}

static void prv_select_click(ClickRecognizerRef recognizer, void *ctx) {
  prv_start_dictation(false);
}

static void prv_up_click(ClickRecognizerRef recognizer, void *ctx) {
  prv_scroll_by(-SCROLL_STEP);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *ctx) {
  prv_scroll_by(SCROLL_STEP);
}

static void prv_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, prv_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, prv_down_click);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_title_layer = text_layer_create(GRect(4, 2, bounds.size.w - 8, 44));
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_overflow_mode(s_title_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_title_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_title_layer));

  GRect scroll_frame = GRect(0, 48, bounds.size.w, bounds.size.h - 48);
  s_scroll = scroll_layer_create(scroll_frame);
  scroll_layer_set_callbacks(s_scroll, (ScrollLayerCallbacks) {
    .click_config_provider = prv_click_config,
  });
  scroll_layer_set_click_config_onto_window(s_scroll, window);
  layer_add_child(root, scroll_layer_get_layer(s_scroll));

  prv_render();
}

static void prv_window_appear(Window *window) {
  if (!s_dictate_on_appear) return;
  s_dictate_on_appear = false;
  prv_start_dictation(true);
}

static void prv_window_unload(Window *window) {
  prv_clear_layers();
  scroll_layer_destroy(s_scroll);
  text_layer_destroy(s_title_layer);
  s_scroll = NULL;
  s_title_layer = NULL;
}

void detail_window_push(bool dictate_immediately) {
  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = prv_window_load,
      .appear = prv_window_appear,
      .unload = prv_window_unload,
    });
    s_dictation = dictation_session_create(DICTATION_BUF_SIZE, prv_dictation_cb, NULL);
  }
  s_scroll_offset = 0;
  s_dictate_on_appear = dictate_immediately;
  prv_render();
  window_stack_push(s_window, true);
}

void detail_window_refresh(void) {
  prv_render();
}

void detail_window_notify_reply_complete(void) {
  // s_window remains allocated after BACK, so explicitly check that the
  // detail screen is the visible top window before notifying the user.
  if (!s_window || window_stack_get_top_window() != s_window) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Reply completed while detail was hidden");
    return;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "Reply completed in visible thread: vibrating");
  vibes_short_pulse();
}
