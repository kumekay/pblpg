/*
 * permes — phone-side bridge between the Pebble watchapp and hermes-agent.
 *
 * Protocol: see src/c/protocol.h (OP_* codes, shared AppMessage keys).
 * hermes API: OpenAI-compatible gateway API server (platforms.api_server),
 *   GET  /api/sessions                     — list threads
 *   POST /api/sessions                     — create thread
 *   GET  /api/sessions/{id}/messages       — history (latest first)
 *   POST /v1/runs {input, session_id}      — start agent turn (202 + run_id)
 *   GET  /v1/runs/{run_id}                 — poll: status/output/error
 */

var CONFIG_PAGE_URL =
  'https://cdn.jsdelivr.net/gh/kumekay/pblpg@main/permes/config/index.html';
var STORAGE = {
  baseUrl: 'permes_base_url',
  apiKey: 'permes_api_key'
};

var CONFIG = {
  baseUrl: '',
  apiKey: '',
  maxThreads: 16,
  pollMs: 5000,
  runTimeoutMs: 15 * 60 * 1000,
  replyMaxBytes: 13000,
  chunkMaxBytes: 440
};

function loadConfig() {
  try {
    CONFIG.baseUrl = localStorage.getItem(STORAGE.baseUrl) || '';
    CONFIG.apiKey = localStorage.getItem(STORAGE.apiKey) || '';
  } catch (e) { /* no localStorage */ }
}

function saveConfig(baseUrl, apiKey) {
  baseUrl = String(baseUrl || '').trim().replace(/\/+$/, '');
  apiKey = String(apiKey || '').trim();
  if (!/^https:\/\//i.test(baseUrl) || !apiKey) return false;

  try {
    localStorage.setItem(STORAGE.baseUrl, baseUrl);
    localStorage.setItem(STORAGE.apiKey, apiKey);
  } catch (e) {
    log('could not persist configuration');
    return false;
  }
  CONFIG.baseUrl = baseUrl;
  CONFIG.apiKey = apiKey;
  return true;
}

function isConfigured() {
  return !!(CONFIG.baseUrl && CONFIG.apiKey);
}

loadConfig();

// System prompt for threads created from the watch: keep answers watch-sized.
var WATCH_SYSTEM_PROMPT =
  'The user talks to you from a Pebble smartwatch with a tiny screen. ' +
  'Keep replies SHORT (under ~600 characters), plain text, no markdown, no lists of links.';

var OP = {
  LIST: 1, NEW: 2, SEND: 3, OPEN: 4,
  LIST_BEGIN: 10, THREAD: 11, LIST_END: 12, NEW_OK: 13, SEND_OK: 14,
  REPLY: 15, STATUS: 16, ERROR: 17
};
var STATUS = { RUNNING: 1, DONE: 2, FAILED: 3 };

// runId -> { threadId, startedAt }
var runs = {};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

function log(msg) { console.log('[permes] ' + msg); }

function authHeaders() {
  return { 'Authorization': 'Bearer ' + CONFIG.apiKey, 'Content-Type': 'application/json' };
}

function request(method, path, body, cb) {
  if (!isConfigured()) {
    cb('URL and token must be configured first.', null);
    return;
  }
  var xhr = new XMLHttpRequest();
  xhr.open(method, CONFIG.baseUrl + path, true);
  var headers = authHeaders();
  for (var k in headers) xhr.setRequestHeader(k, headers[k]);
  xhr.timeout = 30000;
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      var data = null;
      try { data = JSON.parse(xhr.responseText); } catch (e) { /* keep null */ }
      cb(null, data);
    } else {
      cb('HTTP ' + xhr.status + ' ' + (xhr.responseText || '').slice(0, 120), null);
    }
  };
  xhr.onerror = function () { cb('network error', null); };
  xhr.ontimeout = function () { cb('request timeout', null); };
  xhr.send(body ? JSON.stringify(body) : null);
}

// --- sequential AppMessage send queue (avoid EBUSY drops) -----------------

var sendQueue = [];
var sending = false;

function queueSend(dict) {
  sendQueue.push(dict);
  pump();
}

function pump() {
  if (sending || sendQueue.length === 0) return;
  sending = true;
  var dict = sendQueue.shift();
  Pebble.sendAppMessage(dict,
    function () { sending = false; pump(); },
    function (e) { log('send failed: ' + JSON.stringify(dict)); sending = false; pump(); });
}

function sendError(message) {
  queueSend({ OP: OP.ERROR, TEXT: String(message).slice(0, 200) });
}

// --- text cleaning ----------------------------------------------------------

function cleanText(text) {
  if (!text) return '';
  var t = String(text);
  t = t.replace(/```[\s\S]*?```/g, ' [code] ');          // fenced code
  t = t.replace(/`([^`\n]*)`/g, '$1');                    // inline code
  t = t.replace(/!\[[^\]]*\]\([^)]*\)/g, '');             // images
  t = t.replace(/\[([^\]]*)\]\([^)]*\)/g, '$1');          // links -> text
  t = t.replace(/^#{1,6}\s+/gm, '');                      // headers
  t = t.replace(/(\*\*|__)(.*?)\1/g, '$2');               // bold
  t = t.replace(/(^|[\s(])[*_]([^*_\n]+)[*_](?=[\s).,!?]|$)/g, '$1$2'); // italic
  t = t.replace(/^>\s?/gm, '');                            // quotes
  t = t.replace(/\n{3,}/g, '\n\n');
  return t.trim();
}

function utf8Len(str) {
  var n = 0;
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    if (c <= 0x7f) n += 1;
    else if (c <= 0x7ff) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff) { n += 4; i++; } // surrogate pair
    else n += 3;
  }
  return n;
}

// Truncate to maxBytes without splitting a codepoint.
function truncateBytes(str, maxBytes) {
  if (utf8Len(str) <= maxBytes) return str;
  var out = '';
  var used = 0;
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    var w = (c <= 0x7f) ? 1 : (c <= 0x7ff) ? 2 : (c >= 0xd800 && c <= 0xdbff) ? 4 : 3;
    if (used + w > maxBytes - 16) break; // room for the suffix
    out += str[i];
    if (w === 4) { out += str[i + 1]; i++; }
    used += w;
  }
  return out + '... (truncated)';
}

// Split into chunks of <= chunkMaxBytes UTF-8, codepoint-safe.
// truncateBytes() keeps the total under 8 chunks (watch-side limit).
function chunkText(str) {
  var chunks = [];
  var cur = '';
  var curBytes = 0;
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    var ch = str[i];
    var w;
    if (c <= 0x7f) w = 1;
    else if (c <= 0x7ff) w = 2;
    else if (c >= 0xd800 && c <= 0xdbff) { w = 4; ch = str[i] + str[i + 1]; i++; }
    else w = 3;
    if (curBytes + w > CONFIG.chunkMaxBytes && cur) {
      chunks.push(cur);
      cur = ch;
      curBytes = w;
    } else {
      cur += ch;
      curBytes += w;
    }
  }
  if (cur) chunks.push(cur);
  if (chunks.length === 0) chunks.push('');
  return chunks;
}

function sendReply(threadId, text) {
  var chunks = chunkText(truncateBytes(text, CONFIG.replyMaxBytes));
  for (var i = 0; i < chunks.length; i++) {
    queueSend({ OP: OP.REPLY, THREAD_ID: threadId, INDEX: i, COUNT: chunks.length, TEXT: chunks[i] });
  }
}

// Format a session's message history as a readable plain-text transcript,
// keeping the newest messages within the byte budget.
function buildTranscript(messages) {
  var fmt = [];
  for (var i = 0; i < messages.length; i++) {  // API returns chronological order
    var m = messages[i];
    if (!m || !m.content) continue;
    var c = cleanText(String(m.content));
    if (!c) continue;
    if (m.role === 'user') fmt.push('You: ' + c);
    else if (m.role === 'assistant') fmt.push('Valera: ' + c);
  }
  var out = [];
  var used = 0;
  for (var j = fmt.length - 1; j >= 0; j--) {
    var w = utf8Len(fmt[j]) + 2;
    if (used + w > CONFIG.replyMaxBytes - 40) break;
    out.unshift(fmt[j]);
    used += w;
  }
  var text = out.join('\n\n');
  if (out.length < fmt.length) text = '...(earlier omitted)\n\n' + text;
  return text || '(empty thread)';
}

function sendTranscript(threadId, done) {
  request('GET', '/api/sessions/' + encodeURIComponent(threadId) + '/messages?limit=200',
    null, function (err, data) {
      if (!err && data && data.data) {
        sendReply(threadId, buildTranscript(data.data));
      }
      if (done) done();
    });
}

function sendStatus(threadId, status) {
  queueSend({ OP: OP.STATUS, THREAD_ID: threadId, STATUS: status });
}

// ---------------------------------------------------------------------------
// Thread list
// ---------------------------------------------------------------------------

function activeThreads() {
  var ids = {};
  for (var runId in runs) ids[runs[runId].threadId] = true;
  return ids;
}

function pushThreadList() {
  request('GET', '/api/sessions?limit=40', null, function (err, data) {
    if (err || !data || !data.data) {
      log('list failed: ' + err);
      queueSend({ OP: OP.LIST_BEGIN, COUNT: 0 });
      queueSend({ OP: OP.LIST_END });
      sendError(err || 'no session data');
      return;
    }
    var active = activeThreads();
    var items = [];
    for (var i = 0; i < data.data.length && items.length < CONFIG.maxThreads; i++) {
      var s = data.data[i];
      if (s.archived || s.hidden) continue;
      var title = s.title || s.preview || s.id;
      items.push({ id: s.id, title: String(title).slice(0, 60), active: !!active[s.id] });
    }
    queueSend({ OP: OP.LIST_BEGIN, COUNT: items.length });
    for (var j = 0; j < items.length; j++) {
      queueSend({
        OP: OP.THREAD, INDEX: j, THREAD_ID: items[j].id,
        TITLE: items[j].title, ACTIVE: items[j].active ? 1 : 0
      });
    }
    queueSend({ OP: OP.LIST_END });
  });
}

// ---------------------------------------------------------------------------
// New thread
// ---------------------------------------------------------------------------

function createThread() {
  var now = new Date();
  function pad(n) { return n < 10 ? '0' + n : '' + n; }
  var title = 'Watch ' + pad(now.getDate()) + '.' + pad(now.getMonth() + 1) +
              ' ' + pad(now.getHours()) + ':' + pad(now.getMinutes());
  request('POST', '/api/sessions',
    { title: title, source: 'api_server', system_prompt: WATCH_SYSTEM_PROMPT },
    function (err, data) {
      if (err || !data || !data.session || !data.session.id) {
        sendError(err || 'could not create thread');
        return;
      }
      var id = data.session.id;
      var t = data.session.title || title;
      queueSend({ OP: OP.NEW_OK, THREAD_ID: id, TITLE: t });
    });
}

// ---------------------------------------------------------------------------
// Sending a message -> async run -> poll
// ---------------------------------------------------------------------------

function persistRuns() {
  try { localStorage.setItem('permes_runs', JSON.stringify(runs)); } catch (e) {}
}

function loadRuns() {
  if (loadRuns.loaded) return;
  loadRuns.loaded = true;
  try {
    var raw = localStorage.getItem('permes_runs');
    if (raw) runs = JSON.parse(raw) || {};
  } catch (e) { runs = {}; }
  // Resume polling after a pkjs restart
  for (var runId in runs) {
    schedulePoll(runId, runs[runId].threadId, runs[runId].startedAt);
  }
}

function sendMessage(threadId, text) {
  request('POST', '/v1/runs', { input: text, session_id: threadId },
    function (err, data) {
      if (err || !data || !data.run_id) {
        sendError(err || 'could not start run');
        sendStatus(threadId, STATUS.FAILED);
        return;
      }
      runs[data.run_id] = { threadId: threadId, startedAt: Date.now() };
      persistRuns();
      queueSend({ OP: OP.SEND_OK, THREAD_ID: threadId });
      schedulePoll(data.run_id, threadId, runs[data.run_id].startedAt);
    });
}

function schedulePoll(runId, threadId, startedAt) {
  setTimeout(function () { pollRun(runId, threadId, startedAt); }, CONFIG.pollMs);
}

function pollRun(runId, threadId, startedAt) {
  if (!runs[runId]) return;  // finished or forgotten
  if (Date.now() - startedAt > CONFIG.runTimeoutMs) {
    delete runs[runId];
    persistRuns();
    sendStatus(threadId, STATUS.FAILED);
    sendError('run timed out');
    return;
  }
  request('GET', '/v1/runs/' + runId, null, function (err, data) {
    if (!runs[runId]) return;
    if (err) { schedulePoll(runId, threadId, startedAt); return; }  // transient: retry
    var status = data && data.status;
    if (status === 'completed') {
      delete runs[runId];
      persistRuns();
      sendTranscript(threadId, function () { sendStatus(threadId, STATUS.DONE); });
    } else if (status === 'failed' || status === 'cancelled') {
      delete runs[runId];
      persistRuns();
      sendStatus(threadId, STATUS.FAILED);
      sendError((data && data.error) || 'run ' + status);
    } else {
      schedulePoll(runId, threadId, startedAt);  // queued/running/stopping
    }
  });
}

// ---------------------------------------------------------------------------
// Open thread: fetch latest reply from history
// ---------------------------------------------------------------------------

function openThread(threadId) {
  if (activeThreads()[threadId]) {
    sendStatus(threadId, STATUS.RUNNING);  // a run is in flight; reply comes when done
  }
  sendTranscript(threadId);
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  log('ready');
  if (isConfigured()) {
    loadRuns();
    pushThreadList();
  } else {
    queueSend({ OP: OP.LIST_BEGIN, COUNT: 0 });
    queueSend({ OP: OP.LIST_END });
    sendError('URL and token must be configured first.');
  }
});

Pebble.addEventListener('showConfiguration', function () {
  // The fragment is not sent to the config page host, so the secret stays local
  // to the phone webview while still allowing the form to restore its values.
  var current = encodeURIComponent(JSON.stringify({
    baseUrl: CONFIG.baseUrl,
    apiKey: CONFIG.apiKey
  }));
  Pebble.openURL(CONFIG_PAGE_URL + '#' + current);
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return; // user cancelled

  var data;
  try {
    data = JSON.parse(e.response);
  } catch (plainError) {
    try {
      data = JSON.parse(decodeURIComponent(e.response));
    } catch (encodedError) {
      log('invalid configuration response');
      return;
    }
  }

  if (!data || typeof data !== 'object' || !saveConfig(data.baseUrl, data.apiKey)) {
    log('configuration rejected');
    return;
  }
  log('configuration saved');
  loadRuns();
  pushThreadList();
});

Pebble.addEventListener('appmessage', function (e) {
  var op = e.payload.OP;
  var threadId = e.payload.THREAD_ID;
  var text = e.payload.TEXT;
  switch (op) {
    case OP.LIST: pushThreadList(); break;
    case OP.NEW:  createThread(); break;
    case OP.SEND: if (threadId && text) sendMessage(threadId, text); break;
    case OP.OPEN: if (threadId) openThread(threadId); break;
    default: log('unknown op ' + op);
  }
});
