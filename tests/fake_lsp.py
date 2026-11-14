#!/usr/bin/env python3
"""Tiny deterministic language server for tests/pty_test.py.

Uses UTF-16 positions (to exercise the client's conversion), reports an
error for every "BAD" in a document, completes from a fixed word list,
answers definition with the first "def " in the file, and hover with a
fixed string. It also sends a workspace/configuration request to make
sure the client answers server requests.
"""
import json
import sys

inp, out = sys.stdin.buffer, sys.stdout.buffer
docs = {}


def send(msg):
    body = json.dumps(msg).encode()
    out.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    out.flush()


def read():
    length = None
    while True:
        line = inp.readline()
        if not line:
            sys.exit(0)
        if line in (b"\r\n", b"\n"):
            break
        k, _, v = line.decode().partition(":")
        if k.lower() == "content-length":
            length = int(v)
    return json.loads(inp.read(length))


def u16(s):
    return len(s.encode("utf-16-le")) // 2


def diagnostics(uri):
    diags = []
    for n, line in enumerate(docs[uri].split("\n")):
        i = line.find("BAD")
        while i >= 0:
            c = u16(line[:i])
            diags.append({"range": {"start": {"line": n, "character": c},
                                    "end": {"line": n, "character": c + 3}},
                          "severity": 1, "message": "BAD is not allowed"})
            i = line.find("BAD", i + 1)
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
          "params": {"uri": uri, "diagnostics": diags}})


while True:
    m = read()
    method, mid = m.get("method"), m.get("id")
    p = m.get("params") or {}
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": mid, "result": {"capabilities": {
            "positionEncoding": "utf-16", "textDocumentSync": 1,
            "completionProvider": {}, "definitionProvider": True,
            "hoverProvider": True}}})
        send({"jsonrpc": "2.0", "id": 99, "method": "workspace/configuration",
              "params": {"items": [{"section": "x"}]}})
    elif method in ("textDocument/didOpen", "textDocument/didChange"):
        td = p["textDocument"]
        docs[td["uri"]] = td["text"] if method.endswith("didOpen") \
            else p["contentChanges"][-1]["text"]
        diagnostics(td["uri"])
    elif method == "textDocument/completion":
        items = [{"label": w, "detail": "fruit"} for w in
                 ("apple", "apricot", "banana")]
        send({"jsonrpc": "2.0", "id": mid, "result": {"isIncomplete": False,
                                                      "items": items}})
    elif method == "textDocument/definition":
        uri = p["textDocument"]["uri"]
        for n, line in enumerate(docs[uri].split("\n")):
            if line.startswith("def "):
                rng = {"start": {"line": n, "character": 4},
                       "end": {"line": n, "character": 5}}
                send({"jsonrpc": "2.0", "id": mid,
                      "result": [{"uri": uri, "range": rng}]})
                break
        else:
            send({"jsonrpc": "2.0", "id": mid, "result": None})
    elif method == "textDocument/hover":
        send({"jsonrpc": "2.0", "id": mid,
              "result": {"contents": {"kind": "plaintext",
                                      "value": "hover info"}}})
    elif method == "shutdown":
        send({"jsonrpc": "2.0", "id": mid, "result": None})
    elif method == "exit":
        sys.exit(0)
    elif mid is not None and method:
        send({"jsonrpc": "2.0", "id": mid, "result": None})
