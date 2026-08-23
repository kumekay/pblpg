# permes

A Pebble watchapp for talking to the **hermes agent** (Valera) from the watch.
Speak on the watch (Pebble dictation, recognized by the phone app), the agent
runs on the host, and the reply is rendered on the watch. Multiple hermes
sessions = multiple parallel threads.

Target: **Pebble Time 2 (emery / board "obelix")**, 200×228 color.

## Architecture

```
Watch (C)  ⇄  Phone (Pebble app, src/pkjs)  ⇄  HTTPS  ⇄  hermes api_server
            AppMessage (protocol.h)         https://<your-hermes-front>/
                                                        → mac LAN IP:8642
```

- **Watch side** (`src/c/`): thread list (MenuLayer) + thread view
  (ScrollLayer reply, SELECT = dictation via `DictationSession`).
- **Phone side** (`src/pkjs/index.js`): talks to hermes, polls async runs,
  cleans markdown, chunks replies for the watch.
- **hermes side**: gateway `api_server` platform (OpenAI-compatible REST),
  enabled in `~/.hermes/config.yaml` under `platforms.api_server` with a
  Bearer key. Set the URL and key from the app's gear/settings entry in the
  Pebble phone app; pkjs persists them in its local storage.

### hermes endpoints used

| Purpose | Endpoint |
|---|---|
| Thread list | `GET /api/sessions?limit=40` |
| New thread (with watch-sized system prompt) | `POST /api/sessions` |
| Latest reply when opening a thread | `GET /api/sessions/{id}/messages` |
| Send message (async turn) | `POST /v1/runs {input, session_id}` |
| Poll turn | `GET /v1/runs/{run_id}` → `status`, `output`, `error` |

Sessions persist server-side; `/v1/runs` with `session_id` continues the
thread's history automatically (validated).

### AppMessage protocol

See `src/c/protocol.h` (OP codes) and `messageKeys` in `package.json`
(`OP, THREAD_ID, TEXT, TITLE, INDEX, COUNT, STATUS, ACTIVE`). Replies travel
as ≤8 UTF-8-safe chunks of ≤440 bytes; the watch assembles them.

## Build & run

```sh
pebble build
pebble install --emulator emery     # boots QEMU + pypkjs (real pkjs runtime!)
pebble logs --emulator emery        # C + pkjs logs
```

On a phone, open the gear beside **permes**, enter the public HTTPS hermes URL
and Bearer key, then tap **Save**. The hosted page is
`permes/config/index.html`; pkjs handles `showConfiguration` and
`webviewclosed`, validates the response, and stores both values locally.

### Testing the full flow in QEMU (no mic on the emulator)

```sh
pebble emu-button click select --emulator emery   # start dictation on a thread
pebble transcribe "your message" --emulator emery # inject the transcription
pebble emu-button click select --emulator emery   # confirm → sends to hermes
pebble screenshot --no-open --emulator emery shot.png
```

### Pointing pkjs at a local hermes (QEMU testing)

pkjs reads its configuration from localStorage; pypkjs persists it to
`~/Library/Application Support/Pebble SDK/4.33.1/emery/localstorage/<uuid>`
(dbm.dumb format). Seed before booting the emulator:

```python
import dbm.dumb
d = dbm.dumb.open("<...>/localstorage/2d1ee2c8-d0a5-415f-a7e0-213a7250e42b", "c")
d["permes_base_url"] = "http://127.0.0.1:8642"
d["permes_api_key"] = "YOUR_API_SERVER_KEY"
d.close()
```

Why not just use the public HTTPS front in QEMU? macOS Sequoia+
Local Network Privacy blocks *third-party* binaries (like pypkjs's uv
Python) from connecting to private/LAN IPs — connections die with
`EHOSTUNREACH` while system curl/nc work. Loopback is exempt, hence the
override. Real phones are unaffected (the Pebble app does the networking
with user-granted permissions). Alternatively grant Local Network access
in System Settings → Privacy & Security → Local Network to the app that
spawns the pebble tool.

## UX notes

- SELECT on a thread = speak; UP/DOWN scroll the reply; BACK to the list.
- Threads created from the watch get `system_prompt` asking for short,
  plain-text replies (watch-sized). Existing threads (e.g. telegram) get
  markdown-stripped, truncated replies.
- Replies for the open thread render live; other threads keep running in
  parallel (pkjs polls each run; list shows "thinking..." via ACTIVE flag).
