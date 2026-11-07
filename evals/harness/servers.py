"""
Local HTTP servers used by eval cases.

FaultProxy  sits between boris and the model and injects failures, one
            plan entry per model request (then passes everything through):
              ok          forward unchanged
              429         Too Many Requests (Retry-After: 1)
              500/502/503 server error with a JSON error body
              drop        close the connection without a response
              truncate    forward, then return only the first half of the body
              garbage     200 with an HTML error page instead of JSON
              slow:N      wait N seconds, then forward
StaticServer serves fixed pages (prompt-injection tests for fetch).
FakeModel   an OpenAI-compatible model that replays a script, so the
            harness can test itself without a real model.
"""

from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class _Server:
    handler_class = BaseHTTPRequestHandler

    def start(self) -> str:
        owner = self

        class Handler(self.handler_class):
            server_owner = owner

            def log_message(self, *args):  # keep the eval output clean
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        return f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def stop(self):
        if getattr(self, "httpd", None):
            self.httpd.shutdown()
            self.httpd.server_close()


def _read_body(handler) -> bytes:
    length = int(handler.headers.get("Content-Length") or 0)
    return handler.rfile.read(length) if length else b""


def _send(handler, status: int, body: bytes, content_type="application/json", extra=None):
    try:
        handler.send_response(status)
        handler.send_header("Content-Type", content_type)
        handler.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            handler.send_header(k, v)
        handler.end_headers()
        handler.wfile.write(body)
    except (BrokenPipeError, ConnectionResetError):
        pass  # the client gave up (timeout): that is the point of slow:N


# ── Fault proxy ──────────────────────────────────────────────────────────────

class _ProxyHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        proxy = self.server_owner
        body = _read_body(self)
        with proxy.lock:
            index = proxy.count
            proxy.count += 1
        action = proxy.plan[index] if index < len(proxy.plan) else "ok"
        proxy.log.append({"request": index + 1, "action": action})

        if action == "drop":
            self.close_connection = True
            return
        if action in ("429", "500", "502", "503"):
            err = json.dumps({"error": {"message": f"injected {action}"}}).encode()
            _send(self, int(action), err, extra={"Retry-After": "1"} if action == "429" else None)
            return
        if action == "garbage":
            _send(self, 200, b"<html><body>502 Bad Gateway</body></html>", "text/html")
            return
        if action.startswith("slow:"):
            time.sleep(float(action.split(":", 1)[1]))

        status, data, ctype = proxy.forward(self.path, body, self.headers)
        if action == "truncate":
            data = data[: len(data) // 2]
        _send(self, status, data, ctype)


class FaultProxy(_Server):
    handler_class = _ProxyHandler

    def __init__(self, upstream: str, plan: list):
        base = upstream.rstrip("/")
        if base.endswith("/v1/chat/completions"):
            base = base[: -len("/v1/chat/completions")]
        self.upstream = base
        self.plan = list(plan)
        self.count = 0
        self.log = []
        self.lock = threading.Lock()

    def forward(self, path: str, body: bytes, headers) -> tuple:
        req = urllib.request.Request(self.upstream + path, data=body, method="POST")
        for h in ("Content-Type", "Authorization", "Accept"):
            if headers.get(h):
                req.add_header(h, headers[h])
        try:
            with urllib.request.urlopen(req, timeout=900) as resp:
                return resp.status, resp.read(), resp.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), e.headers.get("Content-Type", "application/json")
        except Exception as e:  # upstream down: report it as a gateway error
            return 502, json.dumps({"error": {"message": f"proxy: {e}"}}).encode(), "application/json"


# ── Static pages ─────────────────────────────────────────────────────────────

class _StaticHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        pages = self.server_owner.pages
        path = self.path.split("?", 1)[0]
        if path not in pages:
            _send(self, 404, b"not found", "text/plain")
            return
        content = pages[path]
        ctype = ("text/html; charset=utf-8" if path.endswith((".html", "/"))
                 else "application/json" if path.endswith(".json") else "text/plain; charset=utf-8")
        _send(self, 200, content.encode() if isinstance(content, str) else content, ctype)


class StaticServer(_Server):
    handler_class = _StaticHandler

    def __init__(self, pages: dict):
        self.pages = pages


# ── Scripted fake model ──────────────────────────────────────────────────────

class _FakeModelHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        model = self.server_owner
        request = json.loads(_read_body(self) or b"{}")
        model.requests.append(request)
        step = model.script[model.index] if model.index < len(model.script) else {"content": "Done."}
        model.index += 1
        message = {"role": "assistant", "content": step.get("content")}
        finish = "stop"
        if step.get("tool_calls"):
            message["tool_calls"] = [
                {"id": f"call_{model.index}_{i}", "type": "function",
                 "function": {"name": c["name"], "arguments": json.dumps(c.get("arguments", {}))}}
                for i, c in enumerate(step["tool_calls"])]
            finish = "tool_calls"
        reply = {"choices": [{"index": 0, "message": message, "finish_reason": finish}],
                 "usage": {"prompt_tokens": 100, "completion_tokens": 10}}
        _send(self, 200, json.dumps(reply).encode())


class FakeModel(_Server):
    """script: [{"content": "..."} | {"tool_calls": [{"name", "arguments"}]}]"""
    handler_class = _FakeModelHandler

    def __init__(self, script: list):
        self.script = script
        self.index = 0
        self.requests = []
