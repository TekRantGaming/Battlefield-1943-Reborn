"""A local stand in for the report relay (docs/REPORTS.md), for testing the game's Report an issue page.

It answers POST /v1/bf/reports as the real relay will, with the same checks (shape, sizes, links, mentions, rate
limits per install, duplicates, crash signatures), and prints the GitHub API call it WOULD make. It never talks to
GitHub, needs no token and sends nothing anywhere: every "issue" is written to the output folder instead
(issue-<n>.md with the exact title, labels and body, issue-<n>.json with the API request, and the screenshot).

This is also the reference for the real relay (a route on the Cloudflare Worker, on TekRantGaming's word).

    python -I tools/fake_report_relay.py --out runs/report_relay [--port 18490] [--fail] [--slow 5] [--no-log]
    game: BF_REPORT_ENDPOINT=http://127.0.0.1:18490

--fail answers 503 to everything (the game keeps the report and says so); --slow waits that many seconds first.
--no-log takes requests only as large as the relay before the whole log (26 Nov 2026): a report with its whole log
gets 413, and the game sends it again without (report.cpp PostOnce).
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import sys
import threading
import time

REPO = "TekRantGaming/Battlefield-1943-Reborn"
TYPES = {"crash", "bug", "performance", "server", "suggestion"}
MAX_BODY = 60 * 1024          # GitHub's issue body limit is 65,536 characters
MAX_TITLE, MIN_TITLE = 120, 8
MAX_SHOT = 400 * 1024         # the JPEG, before base64
MAX_LOG = 1400 * 1024         # the whole log's gzip, before base64
MAX_LINKS = 5                 # in what the player typed (above "### System")
PER_HOUR, PER_DAY = 3, 10     # per install
PER_IP_DAY = 20
MAX_DUP_COMMENTS = 50


class State:
    def __init__(self, out):
        self.out = out
        self.lock = threading.Lock()
        self.next_issue = 1001
        self.sent = {}           # install -> [times]
        self.by_ip = {}          # salted ip hash -> [times] (the real relay keeps this in memory only)
        self.seen = {}           # (install, content hash) -> time
        self.signatures = {}     # crash signature -> [issue, comments]
        self.salt = os.urandom(16)


def neutralise(text):
    # A report can never ping anyone: @name becomes @<zero width space>name.
    return re.sub(r"@(?=[A-Za-z0-9_-])", "@​", text)


def allowed_html(body):
    # Only <details>, <summary> and the screenshot marker; any other tag is shown as text. Code blocks (``` fences)
    # are left as they are: GitHub shows them as text anyway.
    def fix(m):
        tag = m.group(0)
        if re.fullmatch(r"</?(details|summary)>", tag) or tag in ("<!-- screenshot -->", "<!-- full log -->"):
            return tag
        return tag.replace("<", "&lt;").replace(">", "&gt;")
    parts = body.split("```")
    for i in range(0, len(parts), 2):  # outside the fences
        parts[i] = re.sub(r"<!-- screenshot -->|<!-- full log -->|<[^>\n]{1,200}>", fix, parts[i])
    return "```".join(parts)


class Handler(http.server.BaseHTTPRequestHandler):
    state = None
    fail = False
    no_log = False
    slow = 0.0

    def log_message(self, fmt, *args):
        sys.stderr.write("relay: " + (fmt % args) + "\n")

    def answer(self, status, obj, headers=None):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.slow:
            time.sleep(self.slow)
        if self.path != "/v1/bf/reports":
            return self.answer(404, {"error": "Not here."})
        if self.fail:
            return self.answer(503, {"error": "The report service is down (--fail)."})
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY + MAX_SHOT * 4 // 3 + (0 if self.no_log else MAX_LOG * 4 // 3) + 16 * 1024 or (
                self.no_log and length > 700 * 1024):
            return self.answer(413, {"error": "The report is too large."})
        install = self.headers.get("X-BF-Install", "")
        version = self.headers.get("X-BF-Version", "")
        if not re.fullmatch(r"[0-9a-f-]{36}", install) or not re.fullmatch(r"[0-9A-Za-z.+-]{1,32}", version):
            return self.answer(400, {"error": "The game's headers are missing."})
        try:
            req = json.loads(self.rfile.read(length))
        except ValueError:
            return self.answer(400, {"error": "Not a report."})
        title, body, kind = req.get("title", ""), req.get("body", ""), req.get("type", "")
        if kind not in TYPES or not isinstance(title, str) or not isinstance(body, str):
            return self.answer(400, {"error": "Not a report."})
        if not MIN_TITLE <= len(title) <= MAX_TITLE:
            return self.answer(422, {"error": "The title must be 8 to 120 characters."})
        if len(body) > MAX_BODY:
            return self.answer(413, {"error": "The report is too large."})
        typed = body.split("### System")[0]
        if len(re.findall(r"https?://", typed)) > MAX_LINKS:
            return self.answer(422, {"error": "Too many links in the report."})
        shot = None
        if req.get("screenshot_jpeg"):
            try:
                shot = base64.b64decode(req["screenshot_jpeg"], validate=True)
            except ValueError:
                return self.answer(400, {"error": "The screenshot is not readable."})
            if len(shot) > MAX_SHOT or not shot.startswith(b"\xff\xd8\xff"):
                return self.answer(413, {"error": "The screenshot is too large or not a JPEG."})
        log = None
        if req.get("log_gz"):
            if self.no_log:
                return self.answer(413, {"error": "The report is too large."})
            try:
                log = base64.b64decode(req["log_gz"], validate=True)
            except ValueError:
                return self.answer(400, {"error": "The log is not readable."})
            if len(log) > MAX_LOG or not log.startswith(b"\x1f\x8b"):
                return self.answer(413, {"error": "The log is too large or not gzip."})

        st = self.state
        now = time.time()
        ip = hashlib.sha256(st.salt + self.client_address[0].encode()).hexdigest()[:16]
        content = hashlib.sha256((title + "\n" + typed).encode()).hexdigest()
        with st.lock:
            times = [t for t in st.sent.get(install, []) if now - t < 86400]
            ip_times = [t for t in st.by_ip.get(ip, []) if now - t < 86400]
            if len([t for t in times if now - t < 3600]) >= PER_HOUR or len(times) >= PER_DAY or len(ip_times) >= PER_IP_DAY:
                return self.answer(429, {"error": "Too many reports for now; it is kept and sent later."}, {"Retry-After": "3600"})
            if now - st.seen.get((install, content), 0) < 86400:
                return self.answer(409, {"error": "This report was already sent today."})
            st.seen[(install, content)] = now
            st.sent[install] = times + [now]
            st.by_ip[ip] = ip_times + [now]
            sig = req.get("signature", "")
            if sig and sig in st.signatures and st.signatures[sig][1] < MAX_DUP_COMMENTS:
                number = st.signatures[sig][0]
                st.signatures[sig][1] += 1
                comment = f"Another player hit this crash: version {version}.\n\n" + body.split("### System", 1)[-1][:4000]
                self.write_call(f"POST /repos/{REPO}/issues/{number}/comments", {"body": neutralise(comment)}, f"comment-{number}-{st.signatures[sig][1]}")
                return self.answer(200, {"duplicate_of": number, "url": f"https://github.com/{REPO}/issues/{number}"})
            number = st.next_issue
            st.next_issue += 1
            if sig:
                st.signatures[sig] = [number, 0]

        body = allowed_html(neutralise(body))
        log_link = ""
        if log:
            name = f"issue-{number}-log.txt.gz"
            with open(os.path.join(st.out, name), "wb") as f:
                f.write(log)
            log_link = f"[The whole log](https://reports.example/logs/{name}) (anonymized, kept 30 days)"
        body = body.replace("<!-- full log -->", log_link)
        if shot:
            name = f"issue-{number}-screenshot.jpg"
            with open(os.path.join(st.out, name), "wb") as f:
                f.write(shot)
            link = f"![Screenshot](https://reports.example/screenshots/{name})"  # the real relay: its own store's link
            body = body.replace("<!-- screenshot -->", link)
        labels = [kind, "from-game", "needs-triage"]
        call = {"title": neutralise(title), "body": body, "labels": labels}
        self.write_call(f"POST /repos/{REPO}/issues", call, f"issue-{number}")
        with open(os.path.join(st.out, f"issue-{number}.md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"# {call['title']}\n\nLabels: {', '.join(labels)}\n\n{body}")
        print(f"relay: WOULD CREATE issue #{number}: {call['title']!r} labels {labels}, body {len(body)} chars"
              f"{', screenshot ' + str(len(shot)) + ' bytes' if shot else ''} -> {st.out}\\issue-{number}.md", flush=True)
        return self.answer(201, {"issue": number, "url": f"https://github.com/{REPO}/issues/{number}"})

    def write_call(self, what, payload, name):
        with open(os.path.join(self.state.out, name + ".json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump({"request": what + " (api.github.com, with the relay's token: NOT sent, this is a fake)", "json": payload}, f,
                      indent=2, ensure_ascii=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=18490)
    ap.add_argument("--fail", action="store_true")
    ap.add_argument("--no-log", action="store_true")
    ap.add_argument("--slow", type=float, default=0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    Handler.state = State(a.out)
    Handler.fail, Handler.slow, Handler.no_log = a.fail, a.slow, a.no_log
    server = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"relay: fake report relay on http://127.0.0.1:{a.port} (nothing goes to GitHub), writing to {a.out}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
