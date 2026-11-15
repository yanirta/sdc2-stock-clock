#!/usr/bin/env python3
"""Claude Code hook: show "Claude needs you" on the Stock Clock.

Claude Code runs this with the hook event as JSON on stdin.
- Notification (permission prompt or waiting for input): an alert page in Claude's theme
- Stop (Claude finished its turn): an alert page that says it's your turn
- UserPromptSubmit, PostToolUse, SessionEnd: clear that session's page

Each session gets its own page (id "cc-<session>"). A marker file per active page lets the frequent
PostToolUse hook exit at once when there is nothing to clear. Never blocks Claude: every failure is ignored.

Clock address: $STOCK_CLOCK_IP (required), e.g. STOCK_CLOCK_IP=192.168.1.50.
"""
import json
import os
import sys
import unicodedata
import urllib.parse
import urllib.request

CLOCK = os.environ.get("STOCK_CLOCK_IP", "")
STATE = os.path.expanduser("~/Library/Application Support/StockClock/active")


def ascii_only(s, n):
    """The clock's fonts are ASCII: transliterate accents, map common punctuation, drop the rest."""
    s = (s or "").replace("—", "-").replace("–", "-").replace("’", "'").replace("‘", "'")
    s = s.replace("“", '"').replace("”", '"').replace("…", "...")
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return " ".join(s.split())[:n]


def send(fields):
    if not CLOCK:
        return
    data = urllib.parse.urlencode(fields).encode()
    try:
        urllib.request.urlopen(f"http://{CLOCK}/api/push", data=data, timeout=1.5).read()
    except Exception:
        pass


def main():
    try:
        ev = json.load(sys.stdin)
    except ValueError:
        return
    name = ev.get("hook_event_name", "")
    sid = (ev.get("session_id") or "x").replace("-", "")[:8]
    pid = "cc-" + sid
    marker = os.path.join(STATE, pid)
    if name in ("UserPromptSubmit", "PostToolUse", "SessionEnd"):
        if os.path.exists(marker):
            os.remove(marker)
            send({"id": pid, "clear": 1})
        return
    project = ascii_only(os.path.basename((ev.get("cwd") or "").rstrip("/")) or "Claude Code", 60)
    if name == "Notification":
        msg = ev.get("message") or "Claude is waiting for your input"
        fields = {"title": "Claude needs you", "l1": project, "l2": ascii_only(msg, 63), "ttl": 3600}
    elif name == "Stop":
        fields = {"title": "Claude is done", "l1": project, "l2": "Your turn: review and reply", "ttl": 1800}
    else:
        return
    fields.update({"id": pid, "theme": "claude", "alert": 1})
    os.makedirs(STATE, exist_ok=True)
    open(marker, "w").close()
    send(fields)


if __name__ == "__main__":
    main()
