#!/usr/bin/env python3
"""Talks to the language server the way an editor does. It starts it, opens a program,
and checks the diagnostics, hover, go to definition, document symbols and completion it answers.

    python3 tests/tools/lspcheck.py [build/avrasm-lsp]

Prints one line per check and exits 1 if any failed."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

SERVER = sys.argv[1] if len(sys.argv) > 1 else "build/avrasm-lsp" + (".exe" if os.name == "nt" else "")
failures = 0


def check(what, ok, got=None):
    global failures
    print(("ok    " if ok else "FAIL  ") + what + ("" if ok else f"\n      got: {got}"))
    failures += not ok


class Client:
    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next = 1
        self.notes = []

    def send(self, msg):
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def read(self):
        length = None
        while True:
            line = self.p.stdout.readline()
            if line in (b"\r\n", b""):
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.p.stdout.read(length)) if length is not None else None

    def request(self, method, params):
        rid = self.next
        self.next += 1
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            msg = self.read()
            if msg is None or msg.get("id") == rid:
                return msg
            self.notes.append(msg)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def diagnostics(self, uri):
        """The diagnostics published last for uri, after a round trip makes sure they have come."""
        self.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        found = None
        for n in self.notes:
            if n.get("method") == "textDocument/publishDiagnostics" and n["params"]["uri"] == uri:
                found = n["params"]["diagnostics"]
        return found


with tempfile.TemporaryDirectory() as tmp:
    main = os.path.join(tmp, "main.asm")
    lib = os.path.join(tmp, "lib.inc")
    with open(lib, "w") as f:
        f.write("; routines\nwait:\n  dec counter\n  brne wait\n  ret\n")
    text = (".def counter = r16\n.equ COUNT = 10\n"
            "start:\n  ldi counter, COUNT\n  rcall wait\n  rjmp start\n"
            ".include \"lib.inc\"\n")
    with open(main, "w") as f:
        f.write(text)
    uri = pathlib.Path(main).as_uri()
    c = Client([SERVER])
    init = c.request("initialize", {"processId": None, "rootUri": pathlib.Path(tmp).as_uri(), "capabilities": {}})
    check("initialize: the server says it can hover", init["result"]["capabilities"].get("hoverProvider") is True, init)
    c.notify("initialized", {})
    c.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "avr", "version": 1, "text": text}})
    check("a good program has no diagnostics", c.diagnostics(uri) == [], c.diagnostics(uri))

    bad = text.replace("ldi counter, COUNT", "ldi counter, COUNTT")
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": bad}]})
    d = c.diagnostics(uri)
    check("an unknown name is an error on line 4, columns 15..21",
          d is not None and len(d) == 1 and d[0]["severity"] == 1 and
          d[0]["range"] == {"start": {"line": 3, "character": 15}, "end": {"line": 3, "character": 21}}, d)
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": text}]})
    check("fixing it clears the error", c.diagnostics(uri) == [], c.diagnostics(uri))

    h = c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 17}})
    check("hover on COUNT shows its value", "= 10" in json.dumps(h["result"]), h)
    h = c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 3}})
    check("hover on ldi shows its bytes", "`0a`" in json.dumps(h["result"]) and "`e0`" in json.dumps(h["result"]), h)
    loc = c.request("textDocument/definition", {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 9}})
    check("go to definition of wait lands in lib.inc, line 2",
          loc["result"] and loc["result"]["uri"].endswith("/lib.inc") and loc["result"]["range"]["start"]["line"] == 1, loc)
    syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    check("document symbols are counter, COUNT and start",
          [s["name"] for s in syms["result"]] == ["start", "COUNT", "counter"] or
          sorted(s["name"] for s in syms["result"]) == ["COUNT", "counter", "start"], syms)
    comp = c.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}})
    labels = {i["label"] for i in comp["result"]}
    check("completion offers ldi, .equ and wait", {"ldi", ".equ", "wait"} <= labels, sorted(labels)[:20])

    # Open the include file too: it is shown as part of main.asm's program, so `counter` is known.
    luri = pathlib.Path(lib).as_uri()
    c.notify("textDocument/didOpen", {"textDocument": {"uri": luri, "languageId": "avr", "version": 1,
                                                        "text": open(lib).read()}})
    check("lib.inc, opened, is checked as part of main.asm", c.diagnostics(luri) == [], c.diagnostics(luri))
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": luri, "version": 2}, 
        "contentChanges": [{"text": "wait:\n  dec counter\n  brne wait\n  ret\n  ret\nwait:\n"
    }]})
    d = c.diagnostics(luri)
    check("an error in lib.inc shows in lib.inc", d is not None and len(d) >= 1 and d[0]["range"]["start"]["line"] == 5, d)

    r = c.request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {}})
    check("an unknown request is MethodNotFound", r.get("error", {}).get("code") == -32601, r)
    r = c.request("shutdown", None)
    check("shutdown answers null", "result" in r and r["result"] is None, r)
    c.notify("exit", None)
    check("exit after shutdown is status 0", c.p.wait(timeout=5) == 0, c.p.returncode)

sys.exit(1 if failures else 0)