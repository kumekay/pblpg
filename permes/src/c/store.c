#include "store.h"
#include "protocol.h"

static Thread s_threads[MAX_THREADS];
static int s_num_threads = 0;
static int s_open_index = -1;
static char s_error[201];

// Reply assembly (chunked)
static char s_chunks[REPLY_CHUNK_MAX][512];
static bool s_chunk_recv[REPLY_CHUNK_MAX];
static int s_chunk_total = 0;
static char s_reply[REPLY_MAX + 1];
static bool s_reply_pending = false;
static int s_scroll_hint = SCROLL_HINT_NONE;

static StoreRefreshCb s_list_cb = NULL;
static StoreRefreshCb s_detail_cb = NULL;

// ---------------------------------------------------------------------------
// Outbox (with retry on EBUSY)
// ---------------------------------------------------------------------------

#define OUT_MAX_TUPLETS 5
static Tuplet s_out_tuplets[OUT_MAX_TUPLETS];
static int s_out_count = 0;
static AppTimer *s_out_retry_timer = NULL;

static bool prv_out_send(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return false;
  for (int i = 0; i < s_out_count; i++) {
    Tuplet *t = &s_out_tuplets[i];
    switch (t->type) {
      case TUPLE_UINT:
      case TUPLE_INT:
        if (t->integer.width <= 1) {
          dict_write_uint8(iter, t->key, (uint8_t)t->integer.storage);
        } else {
          dict_write_uint16(iter, t->key, (uint16_t)t->integer.storage);
        }
        break;
      case TUPLE_CSTRING: dict_write_cstring(iter, t->key, t->cstring.data); break;
      default: break;
    }
  }
  return app_message_outbox_send() == APP_MSG_OK;
}

static void prv_out_retry(void *ctx) {
  s_out_retry_timer = NULL;
  if (!prv_out_send() && !s_out_retry_timer) {
    s_out_retry_timer = app_timer_register(200, prv_out_retry, NULL);
  }
}

static void prv_out_send_tuplets(Tuplet *tuplets, int count) {
  if (count > OUT_MAX_TUPLETS) count = OUT_MAX_TUPLETS;
  if (s_out_retry_timer) {
    app_timer_cancel(s_out_retry_timer);
    s_out_retry_timer = NULL;
  }
  memcpy(s_out_tuplets, tuplets, sizeof(Tuplet) * count);
  s_out_count = count;
  if (!prv_out_send() && !s_out_retry_timer) {
    s_out_retry_timer = app_timer_register(200, prv_out_retry, NULL);
  }
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

void store_request_list(void) {
  Tuplet t = TupletInteger(MESSAGE_KEY_OP, (uint8_t)OP_LIST);
  prv_out_send_tuplets(&t, 1);
}

void store_request_new(void) {
  Tuplet t = TupletInteger(MESSAGE_KEY_OP, (uint8_t)OP_NEW);
  prv_out_send_tuplets(&t, 1);
}

void store_request_open(const char *id) {
  Tuplet ts[2] = {
    TupletInteger(MESSAGE_KEY_OP, (uint8_t)OP_OPEN),
    TupletCString(MESSAGE_KEY_THREAD_ID, id),
  };
  prv_out_send_tuplets(ts, 2);
}

void store_send_message(const char *id, const char *text) {
  Tuplet ts[3] = {
    TupletInteger(MESSAGE_KEY_OP, (uint8_t)OP_SEND),
    TupletCString(MESSAGE_KEY_THREAD_ID, id),
    TupletCString(MESSAGE_KEY_TEXT, text),
  };
  prv_out_send_tuplets(ts, 3);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

int store_count(void) { return s_num_threads; }
const char *store_error(void) { return s_error; }

Thread *store_get(int index) {
  if (index < 0 || index >= s_num_threads) return NULL;
  return &s_threads[index];
}

int store_find(const char *id) {
  for (int i = 0; i < s_num_threads; i++) {
    if (strncmp(s_threads[i].id, id, THREAD_ID_LEN) == 0) return i;
  }
  return -1;
}

Thread *store_open_thread(void) {
  if (s_open_index < 0 || s_open_index >= s_num_threads) return NULL;
  return &s_threads[s_open_index];
}

void store_set_open(int index) {
  s_open_index = index;
  s_reply[0] = '\0';
  s_reply_pending = false;
  s_chunk_total = 0;
  memset(s_chunk_recv, 0, sizeof(s_chunk_recv));
}

const char *store_reply(void) { return s_reply; }
bool store_reply_pending(void) { return s_reply_pending; }

int store_scroll_hint(void) {
  int h = s_scroll_hint;
  s_scroll_hint = SCROLL_HINT_NONE;
  return h;
}

void store_append_you(const char *text) {
  size_t cur = strlen(s_reply);
  size_t room = sizeof(s_reply) - cur;
  if (room < 8) return;
  int w = snprintf(s_reply + cur, room, "%sYou: %s\n\n", cur > 0 ? "\n" : "", text);
  if (w < 0) s_reply[cur] = '\0';
}

void store_mark_failed(const char *error) {
  s_reply_pending = false;
  snprintf(s_reply, sizeof(s_reply), "! %s", error ? error : "Something went wrong");
}

// ---------------------------------------------------------------------------
// Reply assembly
// ---------------------------------------------------------------------------

static void prv_finish_reply(void) {
  s_reply[0] = '\0';
  size_t off = 0;
  for (int i = 0; i < s_chunk_total; i++) {
    size_t len = strlen(s_chunks[i]);
    if (off + len >= REPLY_MAX) {
      size_t room = REPLY_MAX - off;
      memcpy(s_reply + off, s_chunks[i], room);
      off += room;
      break;
    }
    memcpy(s_reply + off, s_chunks[i], len);
    off += len;
  }
  s_reply[off] = '\0';
  s_reply_pending = false;
}

// ---------------------------------------------------------------------------
// Inbox handling
// ---------------------------------------------------------------------------

static Tuple *t(DictionaryIterator *it, uint32_t key) {
  return dict_find(it, key);
}

static void prv_inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *op_t = t(iter, MESSAGE_KEY_OP);
  if (!op_t) return;
  uint8_t op = op_t->value->uint8;

  switch (op) {
    case OP_LIST_BEGIN: {
      Tuple *count_t = t(iter, MESSAGE_KEY_COUNT);
      int count = count_t ? (int)count_t->value->uint16 : 0;
      if (count > MAX_THREADS) count = MAX_THREADS;
      s_num_threads = 0;
      s_error[0] = '\0';
      memset(s_threads, 0, sizeof(s_threads));
      if (count == 0 && s_list_cb) s_list_cb();
      break;
    }

    case OP_THREAD: {
      Tuple *idx_t = t(iter, MESSAGE_KEY_INDEX);
      Tuple *id_t = t(iter, MESSAGE_KEY_THREAD_ID);
      Tuple *title_t = t(iter, MESSAGE_KEY_TITLE);
      Tuple *active_t = t(iter, MESSAGE_KEY_ACTIVE);
      if (!idx_t || !id_t) break;
      int idx = (int)idx_t->value->uint16;
      if (idx < 0 || idx >= MAX_THREADS) break;
      Thread *th = &s_threads[idx];
      memset(th, 0, sizeof(Thread));
      th->used = true;
      strncpy(th->id, id_t->value->cstring, THREAD_ID_LEN - 1);
      if (title_t) strncpy(th->title, title_t->value->cstring, THREAD_TITLE_LEN - 1);
      th->active = active_t ? active_t->value->uint8 != 0 : false;
      if (idx + 1 > s_num_threads) s_num_threads = idx + 1;
      if (s_list_cb) s_list_cb();
      break;
    }

    case OP_LIST_END:
      if (s_list_cb) s_list_cb();
      break;

    case OP_NEW_OK: {
      Tuple *id_t = t(iter, MESSAGE_KEY_THREAD_ID);
      Tuple *title_t = t(iter, MESSAGE_KEY_TITLE);
      if (!id_t) break;
      int idx = store_find(id_t->value->cstring);
      if (idx < 0 && s_num_threads < MAX_THREADS) {
        idx = s_num_threads++;
        memset(&s_threads[idx], 0, sizeof(Thread));
      }
      if (idx < 0) break;
      Thread *th = &s_threads[idx];
      th->used = true;
      strncpy(th->id, id_t->value->cstring, THREAD_ID_LEN - 1);
      if (title_t) strncpy(th->title, title_t->value->cstring, THREAD_TITLE_LEN - 1);
      store_set_open(idx);
      if (s_list_cb) s_list_cb();
      if (s_detail_cb) s_detail_cb();
      break;
    }

    case OP_SEND_OK:
    case OP_STATUS: {
      Tuple *id_t = t(iter, MESSAGE_KEY_THREAD_ID);
      if (!id_t) break;
      int idx = store_find(id_t->value->cstring);
      if (idx < 0) break;
      bool running = (op == OP_SEND_OK);
      if (op == OP_STATUS) {
        Tuple *st_t = t(iter, MESSAGE_KEY_STATUS);
        uint8_t st = st_t ? st_t->value->uint8 : STATUS_RUNNING;
        running = (st == STATUS_RUNNING);
        if (!running && st == STATUS_DONE && idx == s_open_index) {
          s_scroll_hint = SCROLL_HINT_BOTTOM;  // show the fresh answer
        }
      }
      s_threads[idx].active = running;
      if (!running) s_reply_pending = false;
      if (s_list_cb) s_list_cb();
      if (s_detail_cb) s_detail_cb();
      break;
    }

    case OP_REPLY: {
      Tuple *id_t = t(iter, MESSAGE_KEY_THREAD_ID);
      Tuple *idx_t = t(iter, MESSAGE_KEY_INDEX);
      Tuple *count_t = t(iter, MESSAGE_KEY_COUNT);
      Tuple *text_t = t(iter, MESSAGE_KEY_TEXT);
      if (!id_t || !idx_t || !count_t || !text_t) break;
      int idx = store_find(id_t->value->cstring);
      if (idx < 0 || idx != s_open_index) break;  // only render the open thread
      int ci = (int)idx_t->value->uint16;
      int total = (int)count_t->value->uint16;
      if (total < 1 || total > REPLY_CHUNK_MAX || ci < 0 || ci >= total) break;
      if (ci == 0 || total != s_chunk_total) {
        s_chunk_total = total;
        memset(s_chunk_recv, 0, sizeof(s_chunk_recv));
      }
      strncpy(s_chunks[ci], text_t->value->cstring, sizeof(s_chunks[ci]) - 1);
      s_chunks[ci][sizeof(s_chunks[ci]) - 1] = '\0';
      s_chunk_recv[ci] = true;
      bool complete = true;
      for (int i = 0; i < s_chunk_total; i++) {
        if (!s_chunk_recv[i]) { complete = false; break; }
      }
      if (complete) {
        prv_finish_reply();
        s_scroll_hint = SCROLL_HINT_TOP;  // reading a transcript: start at top
        if (s_detail_cb) s_detail_cb();
      }
      break;
    }

    case OP_ERROR: {
      Tuple *text_t = t(iter, MESSAGE_KEY_TEXT);
      const char *error = text_t ? text_t->value->cstring : "Something went wrong";
      strncpy(s_error, error, sizeof(s_error) - 1);
      s_error[sizeof(s_error) - 1] = '\0';
      store_mark_failed(error);
      if (s_detail_cb) s_detail_cb();
      if (s_list_cb) s_list_cb();
      break;
    }

    default:
      break;
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "AppMessage inbox dropped: %d", reason);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void store_set_list_cb(StoreRefreshCb cb) { s_list_cb = cb; }
void store_set_detail_cb(StoreRefreshCb cb) { s_detail_cb = cb; }

void store_init(void) {
  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
  store_request_list();
}

void store_deinit(void) {
  if (s_out_retry_timer) {
    app_timer_cancel(s_out_retry_timer);
    s_out_retry_timer = NULL;
  }
}
