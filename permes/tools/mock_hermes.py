#!/usr/bin/env python3
"""Tiny fake hermes api_server for QEMU screenshot/testing sessions.

Serves canned, English-only threads so emulator screenshots never depend on
(real, possibly non-English) agent data. Implements just the endpoints
permes' pkjs uses:

  GET  /api/sessions                 -> session list (newest first)
  GET  /api/sessions/{id}            -> session (title refresh)
  GET  /api/sessions/{id}/messages   -> chronological messages
  POST /api/sessions                 -> create session
  POST /v1/runs                      -> start run (immediately "completed")
  GET  /v1/runs/{id}                 -> run status

Usage:  python3 tools/mock_hermes.py [port]     (default 8742)
Then point pkjs at http://127.0.0.1:<port> (config.local.js or config page).
"""
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SESSIONS = [
    {
        "id": "mock_hike",
        "title": "Weekend hike essentials",
        "messages": [
            {"role": "user",
             "content": "What are three essentials for a weekend hike?"},
            {"role": "assistant",
             "content": "Water, layered clothing (rain shell included), and "
                        "navigation - a charged phone with offline maps or a "
                        "paper map and compass. Everything else is nice to "
                        "have."},
        ],
    },
    {
        "id": "mock_mountain",
        "title": "Tallest mountain in Europe",
        "messages": [
            {"role": "user", "content": "What is the tallest mountain in Europe?"},
            {"role": "assistant",
             "content": "Mont Blanc - 4,808 m, the highest peak in the Alps "
                        "and Western Europe."},
        ],
    },
    {
        "id": "mock_cycling",
        "title": "Best Czech city for cycling",
        "messages": [
            {"role": "user",
             "content": "Which Czech city has the best cycling infrastructure?"},
            {"role": "assistant",
             "content": "Brno leads on dedicated lanes, with Prague close "
                        "behind thanks to its river paths."},
        ],
    },
    {
        "id": "mock_moon",
        "title": "How many people walked on the Moon",
        "messages": [
            {"role": "user", "content": "How many people have walked on the Moon?"},
            {"role": "assistant",
             "content": "Twelve - all NASA Apollo astronauts, from Armstrong "
                        "and Aldrin in 1969 to Cernan in 1972."},
        ],
    },
    {
        "id": "mock_greeting",
        "title": "Evening greeting and time check",
        "messages": [
            {"role": "user", "content": "Hi, what time is it?"},
            {"role": "assistant",
             "content": "Hello! It is just past six in the evening. Anything "
                        "I can help with?"},
        ],
    },
]


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/api/sessions":
            self._json({"object": "list",
                        "data": [{"id": s["id"], "title": s["title"],
                                  "message_count": len(s["messages"])}
                                 for s in SESSIONS]})
            return
        m = re.fullmatch(r"/api/sessions/([^/]+)/messages", path)
        if m:
            s = next((s for s in SESSIONS if s["id"] == m.group(1)), None)
            self._json({"object": "list", "data": s["messages"] if s else []})
            return
        m = re.fullmatch(r"/api/sessions/([^/]+)", path)
        if m:
            s = next((s for s in SESSIONS if s["id"] == m.group(1)), None)
            self._json({"session": {"id": s["id"], "title": s["title"]} if s else None})
            return
        if re.fullmatch(r"/v1/runs/[^/]+", path):
            self._json({"status": "completed", "output": "Mock reply."})
            return
        self._json({"error": "not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(length)
        path = self.path.split("?")[0]
        if path == "/api/sessions":
            self._json({"session": {"id": "mock_new", "title": ""}})
            return
        if path == "/v1/runs":
            self._json({"run_id": "mock_run"}, 202)
            return
        self._json({"error": "not found"}, 404)

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock_hermes] %s\n" % (fmt % args))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8742
    print("mock hermes on http://127.0.0.1:%d" % port)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
