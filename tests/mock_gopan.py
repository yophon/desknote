"""自测用的假 gopan：只实现 DeskNote 同步用到的几个 MCP 工具，数据放内存里。

行为照 gopan 源码（server/internal/mcpserver）：
- 路径区分大小写，"/" 是根；write_text_file 要求父目录已存在
- 覆盖非空文件必须带 base_sha256，不带报 READ_FIRST，对不上报 EDIT_CONFLICT
- 业务错误以 isError + "CODE: 说明" 返回
- GET /api/events:实时推送(gopan doc/39 五),密钥连上来只收到 data: {"t":"files"}

用法：python mock_gopan.py <端口> <密钥>
"""
import hashlib
import json
import queue
import sys
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Lock

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
KEY = sys.argv[2] if len(sys.argv) > 2 else "test-key"

lock = Lock()
folders = {"/"}
files = {}  # path -> {"id", "content"}
subscribers = set()  # 连着 /api/events 的客户端,每个一个 Queue


def notify():
    for q in list(subscribers):
        q.put("files")


class ToolError(Exception):
    def __init__(self, code, msg):
        super().__init__(code + ": " + msg)


def sha(s):
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def parent(p):
    p = p.rstrip("/")
    i = p.rfind("/")
    return "/" if i <= 0 else p[:i]


def node(path, f):
    return {"id": f["id"], "name": path.rsplit("/", 1)[1], "kind": "file", "size": len(f["content"].encode()),
            "sha256": sha(f["content"]), "path": path, "updated_at": "2026-01-01T00:00:00Z"}


def call(name, a):
    if name == "create_directories":
        p = a["path"].rstrip("/") or "/"
        while p not in folders:
            folders.add(p)
            p = parent(p)
        return {"path": a["path"]}
    if name == "list_files":
        p = a.get("path", "/").rstrip("/") or "/"
        if p not in folders:
            raise ToolError("NOT_FOUND", "没有这个文件夹")
        items = [node(fp, f) for fp, f in sorted(files.items()) if parent(fp) == p]
        return {"items": items, "total": len(items)}
    if name in ("read_text_file", "get_file_info"):
        f = files.get(a["path"])
        if not f:
            raise ToolError("NOT_FOUND", "没有这个文件")
        if name == "get_file_info":
            return node(a["path"], f)
        return {"text": f["content"], "offset": 0, "next_offset": len(f["content"].encode()), "eof": True, "sha256": sha(f["content"])}
    if name == "write_text_file":
        p = a["path"]
        f = files.get(p)
        if f:
            base = a.get("base_sha256", "")
            if not base and f["content"]:
                raise ToolError("READ_FIRST", "这个文件已有内容,先读一遍(拿到 sha256)再覆盖")
            if base and base != sha(f["content"]):
                raise ToolError("EDIT_CONFLICT", "文件在你读之后被改过")
            f["content"] = a["content"]
            return dict(node(p, f), created=False)
        if parent(p) not in folders:
            raise ToolError("NOT_FOUND", "父目录不存在")
        files[p] = {"id": str(uuid.uuid4()), "content": a["content"]}
        return dict(node(p, files[p]), created=True)
    if name == "trash_nodes":
        ids = set(a["node_ids"])
        for p in [p for p, f in files.items() if f["id"] in ids]:
            del files[p]
        return {"success": True}
    raise ToolError("UNKNOWN_TOOL", name)


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path != "/api/events":
            self.send_response(404); self.end_headers(); return
        if self.headers.get("Authorization") != "Bearer " + KEY:
            self.send_response(401); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        q = queue.Queue()
        subscribers.add(q)
        try:
            self.wfile.write(b"retry: 3000\n: ok\n\n")
            self.wfile.flush()
            while True:
                try:
                    q.get(timeout=25)
                    self.wfile.write(b'data: {"t":"files"}\n\n')
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except OSError:
            pass
        finally:
            subscribers.discard(q)

    def do_POST(self):
        if self.path != "/mcp":
            self.send_response(404); self.end_headers(); return
        if self.headers.get("Authorization") != "Bearer " + KEY:
            self.send_response(401); self.end_headers(); return
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])).decode("utf-8"))
        p = req["params"]
        try:
            with lock:
                out = call(p["name"], p.get("arguments") or {})
            if p["name"] in ("write_text_file", "trash_nodes", "create_directories"):
                notify()
            result = {"content": [{"type": "text", "text": json.dumps(out, ensure_ascii=False)}], "structuredContent": out}
        except ToolError as e:
            result = {"content": [{"type": "text", "text": str(e)}], "isError": True}
        body = json.dumps({"jsonrpc": "2.0", "id": req.get("id"), "result": result}, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
