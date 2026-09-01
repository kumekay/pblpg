#include "store.h"
#include "protocol.h"

static Thread s_threads[MAX_THREADS];
static int s_num_threads = 0;
#define OPEN_DRAFT -2

static int s_open_index = -1;
static Thread s_draft;
static char s_error[201];

// Transcript storage: all messages live in one flat pool; the Msg index
// records each message's offset, length and role.
static char s_pool[REPLY_MAX + 1];
static size_t s_pool_len = 0;

typedef struct {
  uint16_t off;
  uint16_t len;
  uint8_t role;
} Msg;

static Msg s_msgs[MSG_MAX];
static int s_num_msgs = 0;

// Reply assembly (chunked): chunks arrive in order (the phone side uses a
// sequential, ACKed send queue), so they are appended straight into the pool
// and no per-chunk buffering is needed.
static bool s_reply_pending = false;
static int s_scroll_hint = SCROLL_HINT_NONE;

static StoreRefreshCb s_list_cb = NULL;
static StoreRefreshCb s_detail_cb = NULL;
static StoreCompletionCb s_completion_cb = NULL;

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

void store_request_open(const char *id) {
  Tuplet ts[2] = {
    TupletInteger(MESSAGE_KEY_OP, (uint8_t)OP_OPEN),
    TupletCString(MESSAGE_KEY_THREAD_ID, id),
  };
  prv_out_send_tuplets(ts, 2);
}

void store_send_message(const char *id, const char *text) {
  if (!id || !id[0]) {
    // A new-thread screen is only a local draft until its first message. The
    // phone creates the remote session and starts the first run atomically
    // from the watch's point of view.
    Tuplet ts[2] = {
      TupletInteger(MESSAGE_KEY_OP, (uint8_t)OP_NEW),
      TupletCString(MESSAGE_KEY_TEXT, text),
    };
    s_draft.active = true;
    prv_out_send_tuplets(ts, 2);
    return;
  }
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
  if (s_open_index == OPEN_DRAFT) return &s_draft;
  if (s_open_index < 0 || s_open_index >= s_num_threads) return NULL;
  return &s_threads[s_open_index];
}

static void prv_clear_transcript(void) {
  s_pool[0] = '\0';
  s_pool_len = 0;
  s_num_msgs = 0;
  s_reply_pending = false;
}

void store_set_open(int index) {
  s_open_index = index;
  prv_clear_transcript();
}

void store_begin_new(void) {
  memset(&s_draft, 0, sizeof(s_draft));
  strncpy(s_draft.title, "New thread", THREAD_TITLE_LEN - 1);
  s_draft.used = true;
  s_open_index = OPEN_DRAFT;
  prv_clear_transcript();
}

int store_msg_count(void) { return s_num_msgs; }

const char *store_msg(int index, int *role_out) {
  if (index < 0 || index >= s_num_msgs) return "";
  if (role_out) *role_out = s_msgs[index].role;
  return s_pool + s_msgs[index].off;
}

bool store_reply_pending(void) { return s_reply_pending; }

int store_scroll_hint(void) {
  int h = s_scroll_hint;
  s_scroll_hint = SCROLL_HINT_NONE;
  return h;
}

// ---------------------------------------------------------------------------
// Transcript helpers
// ---------------------------------------------------------------------------

static bool prv_pool_append(const char *text, size_t len) {
  if (s_pool_len + len + 1 > sizeof(s_pool)) return false;
  memcpy(s_pool + s_pool_len, text, len);
  s_pool_len += len;
  s_pool[s_pool_len] = '\0';
  return true;
}

static void prv_add_msg(uint8_t role, uint16_t off, uint16_t len) {
  if (s_num_msgs >= MSG_MAX) return;
  s_msgs[s_num_msgs].off = off;
  s_msgs[s_num_msgs].len = len;
  s_msgs[s_num_msgs].role = role;
  s_num_msgs++;
}

void store_append_you(const char *text) {
  if (!text || !text[0] || s_num_msgs >= MSG_MAX) return;
  size_t len = strlen(text);
  uint16_t off = (uint16_t)s_pool_len;
  if (!prv_pool_append(text, len)) return;
  prv_add_msg(MSG_ROLE_USER, off, (uint16_t)len);
}

void store_mark_failed(const char *error) {
  s_reply_pending = false;
  Thread *open = store_open_thread();
  if (open) open->active = false;
  char buf[220];
  snprintf(buf, sizeof(buf), "! %s", (error && error[0]) ? error : "Something went wrong");
  size_t len = strlen(buf);
  uint16_t off = (uint16_t)s_pool_len;
  if (!prv_pool_append(buf, len)) return;
  prv_add_msg(MSG_ROLE_SYSTEM, off, (uint16_t)len);
}

// ---------------------------------------------------------------------------
// Reply assembly
// ---------------------------------------------------------------------------

static void prv_finish_reply(void) {
  // The phone side sends the whole session history as a plain-text
  // transcript ("You: ..." / "Agent: ..." blocks separated by blank
  // lines), already appended chunk-by-chunk into s_pool. Split it into
  // role-tagged messages.
  char *p = s_pool;
  while (*p) {
    char *nl = strstr(p, "\n\n");
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (nl) *nl = '\0';
    uint8_t role = MSG_ROLE_AGENT;
    char *body = p;
    if (len > 5 && strncmp(p, "You: ", 5) == 0) {
      role = MSG_ROLE_USER;
      body = p + 5;
      len -= 5;
    } else if (len > 7 && strncmp(p, "Agent: ", 7) == 0) {
      body = p + 7;
      len -= 7;
    }
    if (len > 0) prv_add_msg(role, (uint16_t)(body - s_pool), (uint16_t)len);
    if (!nl) break;
    p = nl + 2;
  }
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
      if (s_open_index == OPEN_DRAFT) {
        // Keep the locally echoed first message while replacing the draft
        // with the newly-created remote session. If the user has already
        // opened another thread, add this one to the list without hijacking it.
        th->active = s_draft.active;
        s_open_index = idx;
      }
      if (s_list_cb) s_list_cb();
      if (s_detail_cb) s_detail_cb();
      break;
    }

    case OP_TITLE: {
      Tuple *id_t = t(iter, MESSAGE_KEY_THREAD_ID);
      Tuple *title_t = t(iter, MESSAGE_KEY_TITLE);
      if (!id_t || !title_t) break;
      int idx = store_find(id_t->value->cstring);
      if (idx < 0) break;
      strncpy(s_threads[idx].title, title_t->value->cstring, THREAD_TITLE_LEN - 1);
      s_threads[idx].title[THREAD_TITLE_LEN - 1] = '\0';
      if (s_list_cb) s_list_cb();
      if (idx == s_open_index && s_detail_cb) s_detail_cb();
      break;
    }

    case OP_SEND_OK:
    case OP_STATUS: {
      Tuple *id_t = t(iter, MESSAGE_KEY_THREAD_ID);
      if (!id_t) break;
      int idx = store_find(id_t->value->cstring);
      if (idx < 0) break;
      bool was_running = s_threads[idx].active;
      bool running = (op == OP_SEND_OK);
      bool completed_open_reply = false;
      if (op == OP_STATUS) {
        Tuple *st_t = t(iter, MESSAGE_KEY_STATUS);
        uint8_t st = st_t ? st_t->value->uint8 : STATUS_RUNNING;
        running = (st == STATUS_RUNNING);
        completed_open_reply = st == STATUS_DONE && was_running && idx == s_open_index;
        if (completed_open_reply) {
          s_scroll_hint = SCROLL_HINT_REPLY;  // start reading the fresh answer
        }
      }
      s_threads[idx].active = running;
      if (!running) s_reply_pending = false;
      if (s_list_cb) s_list_cb();
      if (s_detail_cb) s_detail_cb();
      // Only a running -> done transition is a new completion. This makes
      // retried/duplicate DONE messages harmless.
      if (completed_open_reply && s_completion_cb) s_completion_cb();
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
      if (ci == 0) {
        s_pool[0] = '\0';
        s_pool_len = 0;
        s_num_msgs = 0;
      }
      prv_pool_append(text_t->value->cstring, strlen(text_t->value->cstring));
      if (ci == total - 1) {
        prv_finish_reply();
        // Opening an idle thread starts at the transcript top. For a live
        // turn, OP_STATUS(DONE) follows and targets the newest reply instead.
        s_scroll_hint = s_threads[idx].active ? SCROLL_HINT_NONE : SCROLL_HINT_TOP;
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
void store_set_completion_cb(StoreCompletionCb cb) { s_completion_cb = cb; }

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
