#!/usr/bin/env python3
"""Send your Claude plan usage (5-hour and weekly limit %) to the Stock Clock.

Reads the Claude Code login from the macOS keychain, asks Anthropic for the same numbers that
Claude Code's usage view shows, and POSTs only the percentages and reset times to the clock.
The login token is only ever sent to api.anthropic.com, never to the clock.

The usage endpoint is undocumented and may change. The token is refreshed by Claude Code while
you use it; when it has expired this script does nothing, and the clock shows how old its data is.

Usage: claude_usage_push.py <clock-ip>      (or set $STOCK_CLOCK_IP)
"""
import datetime
import getpass
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request

CLOCK = (sys.argv[1] if len(sys.argv) > 1 else os.environ.get("STOCK_CLOCK_IP", ""))
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"


def log(msg):
    print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}", flush=True)


def login():
    """Returns (access token, plan) from the keychain, or (None, None) if missing or expired."""
    for args in (["-a", getpass.getuser()], []):
        r = subprocess.run(["/usr/bin/security", "find-generic-password", "-s", "Claude Code-credentials", *args, "-w"],
                           capture_output=True, text=True)
        if r.returncode:
            continue
        try:
            oauth = json.loads(r.stdout)["claudeAiOauth"]
        except (ValueError, KeyError):
            continue
        if oauth.get("expiresAt", 0) / 1000 < time.time() + 60:
            log("Claude login has expired; open Claude Code to refresh it")
            return None, None
        return oauth["accessToken"], oauth.get("subscriptionType") or ""
    log("no Claude Code login found in the keychain")
    return None, None


def epoch(iso):
    if not iso:
        return 0
    return int(datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp())


def main():
    if not CLOCK:
        log("usage: claude_usage_push.py <clock-ip>  (or set STOCK_CLOCK_IP)")
        return 2
    token, plan = login()
    if not token:
        return 0
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
        "User-Agent": "claude-code",
    })
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            u = json.load(r)
    except Exception as e:  # network, 401, endpoint changed...
        log(f"usage request failed: {e}")
        return 1
    five, week = u.get("five_hour") or {}, u.get("seven_day") or {}
    if "utilization" not in five or "utilization" not in week:
        log("usage response has no five_hour/seven_day utilization; the endpoint may have changed")
        return 1
    body = urllib.parse.urlencode({
        "s": max(0, min(100, round(five["utilization"]))),
        "sr": epoch(five.get("resets_at")),
        "w": max(0, min(100, round(week["utilization"]))),
        "wr": epoch(week.get("resets_at")),
        "plan": plan[:11],
    }).encode()
    try:
        with urllib.request.urlopen(f"http://{CLOCK}/api/claude", data=body, timeout=10) as r:
            r.read()
    except Exception as e:
        log(f"could not reach the clock at {CLOCK}: {e}")
        return 1
    log(f"sent: 5-hour {round(five['utilization'])}%, weekly {round(week['utilization'])}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
