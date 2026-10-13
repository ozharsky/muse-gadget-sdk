#!/usr/bin/env python3
"""Raise a gadget notification for a specific agent -- the tap on the device then OPENS that agent.

Usage:
    python3 notify.py default  "Disk is getting full"      # opens the Hermes slot
    python3 notify.py sidekick  "The build finished!"         # opens the ALT slot = sidekick
    NOTIF_MOOD=alert python3 notify.py sidekick "Printer done"  # optional mood (see below)
    python3 notify.py sidekick "Tea's ready" --in 8m          # REMINDER: fires in 8 minutes
    python3 notify.py sidekick "Stand up" --at 14:30          # REMINDER: fires at 14:30 today
    python3 notify.py sidekick "Call mom" --at "tomorrow 9:00"

Moods (any the avatar knows): alert, surprised, pleased, smirk, scan, proud, excited, thinking
... (run `>react=<name>` over serial, or see the reaction list, to preview). "alert" is a good
neutral default for a nudge.

A REMINDER is just a notification the device cannot see until its time: the relay hides it from
the poll until `due` passes, then it appears exactly like any other nudge (chirp, mood, tap opens
the agent). No firmware involved. While undelivered it sits in notifications.json; `--cancel`
lists what is waiting.

The agent argument is a profile name from the relay's agents.json; `default` (or an empty
string) means the base Hermes. An unknown name is accepted but the device will open the Hermes
slot instead (the relay refuses to open a profile it cannot route).
"""
import datetime
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
RELAY = os.environ.get("RELAY", "http://127.0.0.1:8788")


def parse_due(args):
    """-> (due_unix_or_None, remaining_args).  Handles --in <dur> and --at <time>.

    --at consumes extra words when the time is unquoted ("--at tomorrow 9:00" arrives as three
    argv entries); a word is part of the spec while it is not a flag and the spec so far cannot
    parse on its own.
    """
    rest, due = [], None
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--in", "--at") and i + 1 < len(args):
            kind = a
            parts = [args[i + 1]]
            i += 2
            while i < len(args) and not args[i].startswith("-") and due_from(kind, " ".join(parts)) is None:
                parts.append(args[i])   # "tomorrow 9:00" / "14:30:00 extra"? keep joining until it parses
                i += 1
            spec = " ".join(parts).strip()
            due = due_from(kind, spec)
            if due is None:
                raise SystemExit("cannot read %s %r (try 8m, 90s, 14:30, 'tomorrow 9:00')" % (kind, spec))
            continue
        if a.startswith("--in=") or a.startswith("--at="):
            kind, spec = a.split("=", 1)
            due = due_from(kind, spec)
            if due is None:
                raise SystemExit("cannot read %r (try 8m, 90s, 14:30, 'tomorrow 9:00')" % a)
            i += 1
            continue
        rest.append(a)
        i += 1
    return due, rest


def due_from(kind, spec):
    now = datetime.datetime.now()
    if kind == "--in":
        m = re.fullmatch(r"(\d+)\s*([smhd])?", spec.lower())
        if not m:
            return None
        n = int(m.group(1))
        unit = m.group(2) or "m"
        secs = n * {"s": 1, "m": 60, "h": 3600, "d": 86400}[unit]
        return int(now.timestamp()) + secs
    # --at: "14:30", "tomorrow 9:00", "2026-10-09 07:15"
    s = spec.lower().replace("tomorrow", "").strip()
    base = now + datetime.timedelta(days=1) if "tomorrow" in spec.lower() else now
    for fmt in ("%H:%M", "%H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d %H:%M:%S"):
        try:
            t = datetime.datetime.strptime(s, fmt)
        except ValueError:
            continue
        when = base.replace(hour=t.hour, minute=t.minute, second=getattr(t, "second", 0), microsecond=0)
        if when <= now:   # a bare "14:30" already past means tomorrow
            when += datetime.timedelta(days=1)
        return int(when.timestamp())
    return None


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    try:
        due, args = parse_due(args)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2
    if len(args) < 2:
        print(__doc__)
        return 2
    agent = args[0]
    text = " ".join(args[1:]).strip()
    if agent in ("hermes",):
        agent = "default"
    try:
        token = open(os.path.join(HERE, "token")).read().strip()
    except FileNotFoundError:
        raise SystemExit("no relay token yet -- start the relay once (it writes ./token)")
    body = {"text": text, "agent": agent, "mood": os.environ.get("NOTIF_MOOD", "")}
    if due:
        body["due"] = due
    req = urllib.request.Request(RELAY + "/notifications",
                                 data=json.dumps(body).encode(), method="POST")
    req.add_header("X-Gadget-Token", token)
    req.add_header("Content-Type", "application/json")
    try:
        r = json.loads(urllib.request.urlopen(req, timeout=10).read())
    except Exception as e:
        print("relay unreachable (%s): is relay.py running on %s?" % (e, RELAY), file=sys.stderr)
        return 1
    n = r["notification"]
    slot = "the Hermes slot" if n["is_default"] else ("the ALT slot = %s" % n["agent"])
    if n.get("due"):
        when = datetime.datetime.fromtimestamp(n["due"]).strftime("%H:%M")
        print("queued for %s (opens %s) at %s: %s" % (n["label"], slot, when, n["text"]))
    else:
        print("queued for %s (opens %s): %s" % (n["label"], slot, n["text"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
