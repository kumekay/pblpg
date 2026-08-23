#include "detail_window.h"
#include "store.h"

#define DICTATION_BUF_SIZE 512
#define SCROLL_STEP 44

static Window *s_window;
static TextLayer *s_title_layer;
static ScrollLayer *s_scroll;
static TextLayer *s_body_layer;
static DictationSession *s_dictation;
static int s_scroll_offset = 0;   // >= 0: pixels scrolled down
static bool s_dictating = false;

static void prv_render(void) {
  Thread *th = store_open_thread();
  if (!th || !s_window || !window_is_loaded(s_window)) return;

  text_layer_set_text(s_title_layer, th->title[0] ? th->title : th->id);

  static char body[REPLY_MAX + 64];
  const char *reply = store_reply();
  if (store_reply_pending() || th->active) {
    snprintf(body, sizeof(body), "thinking...\n\n%s", reply);
  } else if (reply[0]) {
    snprintf(body, sizeof(body), "%s\n\nSELECT: speak", reply);
  } else {
    snprintf(body, sizeof(body), "No reply yet.\n\nPress SELECT and speak to the agent.");
  }
  text_layer_set_text(s_body_layer, body);

  // Resize body + scroll content to fit the text
  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll));
  GSize text_size = graphics_text_layout_get_content_size(
      body, fonts_get_system_font(FONT_KEY_GOTHIC_24),
      GRect(0, 0, frame.size.w - 4, 10000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int16_t content_h = text_size.h + 8;
  if (content_h < frame.size.h) content_h = frame.size.h;
  text_layer_set_size(s_body_layer, GSize(frame.size.w - 4, content_h));
  scroll_layer_set_content_size(s_scroll, GSize(frame.size.w, content_h));

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
  if (status == DictationSessionStatusSuccess && transcription && transcription[0]) {
    prv_send_dictated(transcription);
  }
}

// ---------------------------------------------------------------------------
// Clicks
// ---------------------------------------------------------------------------

static void prv_select_click(ClickRecognizerRef recognizer, void *ctx) {
  if (s_dictating) return;
  Thread *th = store_open_thread();
  if (!th || !th->id[0]) return;  // thread not created yet
  s_dictating = true;
  dictation_session_start(s_dictation);
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

  s_body_layer = text_layer_create(GRect(4, 0, scroll_frame.size.w - 8, 200));
  text_layer_set_font(s_body_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_overflow_mode(s_body_layer, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_body_layer, GColorClear);
  scroll_layer_add_child(s_scroll, text_layer_get_layer(s_body_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll));

  prv_render();
}

static void prv_window_unload(Window *window) {
  scroll_layer_destroy(s_scroll);
  text_layer_destroy(s_body_layer);
  text_layer_destroy(s_title_layer);
  s_scroll = NULL;
  s_body_layer = NULL;
  s_title_layer = NULL;
}

void detail_window_push(void) {
  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers) {
      .load = prv_window_load,
      .unload = prv_window_unload,
    });
    s_dictation = dictation_session_create(DICTATION_BUF_SIZE, prv_dictation_cb, NULL);
  }
  s_scroll_offset = 0;
  prv_render();
  window_stack_push(s_window, true);
}

void detail_window_refresh(void) {
  prv_render();
}
