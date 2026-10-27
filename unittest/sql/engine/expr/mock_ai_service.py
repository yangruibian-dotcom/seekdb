"""Local regression fixture, NOT the competition's official Mock AI Service.

Run: python mock_ai_service.py --host 0.0.0.0 --port 8000
Faults are selected by URL /fault/<mode>/<normal route>, never by model or input.
"""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def embedding(text, dimension):
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return [digest[i % len(digest)] / 256 for i in range(dimension)]


class MockServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address):
        super().__init__(address, Handler)
        self.requests = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value, status=200):
        data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/requests":
            with self.server.lock:
                records = list(self.server.requests)
            self.reply(records)
        else:
            self.reply({"status": "ok"})

    def do_DELETE(self):
        if self.path != "/requests":
            self.reply({"error": "unknown route"}, 404)
        else:
            with self.server.lock:
                self.server.requests.clear()
            self.reply({"ok": True})

    def do_POST(self):
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if size <= 0 or size > 8 * 1024 * 1024:
                raise ValueError("invalid body length")
            body = json.loads(self.rfile.read(size))
            if not isinstance(body, dict) or not isinstance(body.get("model"), str):
                raise ValueError("model must be a string")
            route = self.path
            fault = None
            if route.startswith("/fault/"):
                _, _, fault, route = route.split("/", 3)
                route = "/" + route
            with self.server.lock:
                self.server.requests.append({"route": route, "fault": fault, "body": body})
            if fault == "http":
                self.reply({"error": "injected failure"}, 400)
                return
            if fault == "root":
                self.reply([])
                return
            if route in ("/v1/chat/completions", "/api/generate"):
                if route == "/api/generate":
                    if body.get("stream") is not False:
                        raise ValueError("native Ollama generation must disable streaming")
                    content = body["prompt"]
                else:
                    content = body["messages"][-1]["content"]
                if fault == "empty":
                    content = ""
                if fault == "null":
                    content = None
                if route == "/api/generate":
                    self.reply({"response": content, "done": True})
                else:
                    self.reply({"choices": [{"message": {"content": content}}]})
            elif route in ("/v1/embeddings", "/api/embed"):
                inputs = body["input"]
                if isinstance(inputs, str):
                    inputs = [inputs]
                dimension = body.get("dimensions", 4)
                if not isinstance(inputs, list) or not inputs or not all(isinstance(x, str) for x in inputs):
                    raise ValueError("input must contain strings")
                if type(dimension) is not int or not 0 < dimension <= 4096:
                    raise ValueError("invalid dimension")
                vectors = [embedding(x, dimension) for x in inputs]
                if fault == "dimension":
                    vectors[-1].append(0)
                if fault == "element":
                    vectors[0][0] = "bad"
                if route == "/api/embed":
                    self.reply({"embeddings": vectors})
                else:
                    data = [{"index": i, "embedding": v} for i, v in enumerate(vectors)]
                    if fault == "duplicate" and len(data) > 1:
                        data[-1]["index"] = 0
                    if fault == "missing":
                        data.pop()
                    self.reply({"data": list(reversed(data))})
            elif route == "/v1/rerank":
                docs = body["documents"]
                if not isinstance(docs, list) or not all(isinstance(x, str) for x in docs):
                    raise ValueError("documents must be strings")
                # Deliberately unsorted; equal lengths exercise stable original-index ordering.
                data = [{"index": i, "relevance_score": len(doc) / (len(doc) + 1)} for i, doc in enumerate(docs)]
                if fault == "duplicate" and len(data) > 1:
                    data[-1]["index"] = 0
                if fault == "missing":
                    data.pop()
                self.reply({"results": list(reversed(data))})
            else:
                self.reply({"error": "unknown route"}, 404)
        except (ValueError, KeyError, TypeError, IndexError) as exc:
            self.reply({"error": str(exc)}, 400)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    with MockServer((args.host, args.port)) as server:
        print(f"Regression mock listening on {args.host}:{server.server_port}", flush=True)
        server.serve_forever()
