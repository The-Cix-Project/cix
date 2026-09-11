"""
Screenshot the Cix dashboard, authenticated, over the Chrome DevTools
Protocol.

Needed because the dashboard keeps its bearer token in
localStorage["cix-auth-token"] (web/app.js:639), so a bare
`chromium --headless --screenshot` gets the login screen: the static
assets serve unauthenticated but every API call inside the page does
not. CDP is the smallest way to put a token in place before the app
reads it.

Order matters. Page.addScriptToEvaluateOnNewDocument runs before any
page script, so the token is in localStorage by the time app.js's
module-level read at line 639 happens -- navigating first and setting
it afterwards would need a reload and race the initial render.

Raw websocket framing rather than a library: this sandbox has no
`websockets` module, and wsconsole.py already established the pattern.
CDP here is plain ws on loopback, so no TLS.
"""
import base64, json, os, socket, struct, subprocess, sys, time, urllib.request

HOST = "192.168.15.95"
ROUTE = sys.argv[1] if len(sys.argv) > 1 else "build-overview"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/shot.png"
W, H = (int(sys.argv[3]) if len(sys.argv) > 3 else 1600,
        int(sys.argv[4]) if len(sys.argv) > 4 else 1400)
PORT = 9222 + (os.getpid() % 500)

import ssl
ctx = ssl._create_unverified_context()
pw = open("/tmp/cxpw.txt").read().strip()
r = urllib.request.Request("https://%s/v1/login" % HOST,
    data=json.dumps({"username": "claude", "password": pw}).encode(),
    headers={"Content-Type": "application/json"}, method="POST")
TOK = json.loads(urllib.request.urlopen(r, context=ctx).read())["token"]

prof = "/tmp/cix-shot-profile-%d" % os.getpid()
chrome = subprocess.Popen([
    "/usr/bin/chromium", "--headless=new", "--no-sandbox", "--disable-gpu",
    "--ignore-certificate-errors", "--hide-scrollbars",
    "--remote-debugging-port=%d" % PORT, "--user-data-dir=" + prof,
    "--window-size=%d,%d" % (W, H), "about:blank",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def targets():
    return json.loads(urllib.request.urlopen(
        "http://127.0.0.1:%d/json" % PORT, timeout=3).read())

ws_url = None
for _ in range(60):
    time.sleep(0.5)
    try:
        for t in targets():
            if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                ws_url = t["webSocketDebuggerUrl"]
                break
    except Exception:
        continue
    if ws_url:
        break
if not ws_url:
    chrome.kill(); sys.exit("chromium did not expose a page target")

path = ws_url.split("127.0.0.1:%d" % PORT, 1)[1]
s = socket.create_connection(("127.0.0.1", PORT), timeout=20)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((("GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n") % (path, PORT, key)).encode())
buf = b""
while b"\r\n\r\n" not in buf:
    d = s.recv(4096)
    if not d:
        break
    buf += d
head, _, rest = buf.partition(b"\r\n\r\n")
if b"101" not in head.split(b"\r\n")[0]:
    chrome.kill(); sys.exit("CDP handshake failed: " + head.decode()[:200])

inbuf = bytearray(rest)

def send_frame(text):
    p = text.encode()
    hdr = bytearray([0x81])
    n = len(p)
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126); hdr += struct.pack("!H", n)
    else:
        hdr.append(0x80 | 127); hdr += struct.pack("!Q", n)
    m = os.urandom(4); hdr += m
    s.sendall(bytes(hdr) + bytes(b ^ m[i % 4] for i, b in enumerate(p)))

def read_frame(timeout=30):
    end = time.time() + timeout
    while True:
        while True:
            if len(inbuf) < 2:
                break
            b2 = inbuf[1]
            ln = b2 & 0x7f
            off = 2
            if ln == 126:
                if len(inbuf) < 4:
                    break
                ln = struct.unpack("!H", inbuf[2:4])[0]; off = 4
            elif ln == 127:
                if len(inbuf) < 10:
                    break
                ln = struct.unpack("!Q", inbuf[2:10])[0]; off = 10
            if len(inbuf) < off + ln:
                break
            payload = bytes(inbuf[off:off + ln]); del inbuf[:off + ln]
            return json.loads(payload.decode("utf-8", "replace"))
        if time.time() > end:
            raise TimeoutError("no CDP frame")
        s.settimeout(max(0.2, end - time.time()))
        d = s.recv(1 << 20)
        if not d:
            raise EOFError("CDP closed")
        inbuf.extend(d)

_id = [0]
def call(method, params=None, timeout=40):
    _id[0] += 1
    mine = _id[0]
    send_frame(json.dumps({"id": mine, "method": method, "params": params or {}}))
    while True:
        msg = read_frame(timeout)
        if msg.get("id") == mine:
            if "error" in msg:
                raise RuntimeError("%s: %s" % (method, msg["error"]))
            return msg.get("result", {})

call("Page.enable")
call("Runtime.enable")
# Before any page script: app.js reads the token at module scope.
call("Page.addScriptToEvaluateOnNewDocument", {
    "source": "try{localStorage.setItem('cix-auth-token', %s);}catch(e){}"
              % json.dumps(TOK)})
call("Page.navigate", {"url": "https://%s/#%s" % (HOST, ROUTE)})
time.sleep(9)   # let the route render and its refreshers land
# Nudge the hash in case the app rewrote it during boot.
call("Runtime.evaluate", {"expression":
    "location.hash = '#%s'; typeof refreshBuildOverview === 'function' ? "
    "refreshBuildOverview() : null" % ROUTE, "awaitPromise": False})
time.sleep(4)
shot = call("Page.captureScreenshot", {"format": "png", "captureBeyondViewport": True})
open(OUT, "wb").write(base64.b64decode(shot["data"]))
print("wrote", OUT, os.path.getsize(OUT), "bytes")
# What the page thinks it is showing, so a blank image can be told from a
# failed render.
ev = call("Runtime.evaluate", {"expression":
    "JSON.stringify({hash: location.hash,"
    " tab: (document.querySelector('#view-integration > .tab-bar > .tab-button.active')||{}).textContent,"
    " caps: (document.getElementById('bo-capacity')||{}).childElementCount,"
    " builds: (document.getElementById('bo-builds')||{}).textContent.slice(0,160)})",
    "returnByValue": True})
print("page:", ev.get("result", {}).get("value"))
send_frame(json.dumps({"id": 999, "method": "Browser.close", "params": {}}))
time.sleep(0.5)
chrome.terminate()
