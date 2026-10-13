#!/usr/bin/env python3
"""Pending notifications -- the gadget's *reverse* channel (stdlib only).

Everything else the gadget does is poll: it asks, the relay answers. This module is the other
direction: a short line the device presents *unprompted* -- it looks up, reacts, chirps, and
offers to open the exact agent that raised it.

A notification is::

    {"id": "...", "ts": 169.., "agent": "sidekick", "is_default": false,
     "label": "SIDEKICK", "mood": "alert", "text": "The build finished.", "read": false}

An optional ``due`` (unix ts) turns one into a REMINDER: :func:`pending` hides it until the
clock passes, so "tea in 8 minutes" needs no firmware change at all -- the device's poll
simply does not see it early. ``due`` absent/0 means "now".

``agent`` is the RESOLVED profile name (agents.json). ``is_default`` tells the device which agent
slot to open: True -> the Hermes slot, False -> the ALT slot loaded with ``agent``. The store is
``notifications.json`` beside this file, newest first, capped at ``MAX``.

Agent identity is resolved at CREATION time (:func:`add`) via :mod:`agents`, so a notification
always carries a name the device can open -- an unknown profile is recorded as the default with a
warning, never a name that cannot be routed.
"""
import json
import os
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
STORE = os.path.join(HERE, "notifications.json")
MAX = 50


def _load():
    """-> list of notifications (newest first).  A missing/corrupt file reads as empty."""
    try:
        data = json.load(open(STORE))
        return data if isinstance(data, list) else []
    except (OSError, ValueError):
        return []


def _save(items):
    tmp = STORE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(items[:MAX], f, indent=1)
    os.replace(tmp, STORE)   # atomic: a reader never sees a half-written store


def resolve_agent(name):
    """A requested profile name -> the identity a notification carries.

    -> ``{"agent", "is_default", "label", "usable", "requested"}``.  Empty/``default`` -> the
    Hermes slot.  A KNOWN usable profile -> the ALT slot with that profile.  An unknown or
    unusable name -> the default slot (with a warning): the device must never open a profile
    that cannot be routed.
    """
    import agents
    want = (name or "").strip()
    base = agents.resolve(want)   # never raises; falls back to default on its own
    is_default = (base["name"] == agents.DEFAULT_NAME)
    return {"agent": base["name"], "is_default": is_default,
            "label": base["label"], "usable": base["usable"], "requested": base["requested"]}


def add(text, agent="", mood="", due=None):
    """Queue one notification for `agent` (a profile name; empty -> the default Hermes).

    ``due`` (unix ts, or 0/None) makes it a reminder: :func:`pending` hides it until then.
    """
    text = (text or "").strip()
    if not text:
        return None
    who = resolve_agent(agent)
    item = {"id": "n_" + uuid.uuid4().hex[:12], "ts": int(time.time()),
            "agent": who["agent"], "is_default": who["is_default"], "label": who["label"],
            "mood": (mood or "").strip(), "text": text, "read": False}
    due = int(due or 0)
    if due > 0:
        item["due"] = due
    items = _load()
    items.insert(0, item)
    _save(items)
    return item


def pending():
    """Unread, DUE notifications, newest first -- what the device polls to present.

    A reminder (``due`` in the future) is invisible here until its time passes, so the
    device needs no timer logic: it already polls; the line simply is not there yet.
    """
    now = int(time.time())
    return [n for n in _load() if not n.get("read") and int(n.get("due") or 0) <= now]


def ack(nid):
    """Mark one read so it is not presented again.  -> True when it was found."""
    items = _load()
    hit = False
    for n in items:
        if n.get("id") == nid and not n.get("read"):
            n["read"] = True
            hit = True
    if hit:
        _save(items)
    return hit


def ack_all():
    items = _load()
    for n in items:
        n["read"] = True
    _save(items)


def clear():
    """Wipe the whole store (tests, benches).  The device-facing clear is clear_history."""
    _save([])


def clear_history():
    """Remove everything EXCEPT reminders that have not fired yet -- the gadget's "Clear".

    The inbox is a blend of history (things that happened / were shown) and commitments
    (reminders still to come).  Clearing must empty the noise without silently cancelling a
    reminder the user set minutes ago: un-fired reminders stay.  A reminder that has fired
    behaves like any other history line from then on.  -> how many reminders were kept.
    """
    now = int(time.time())
    items = _load()
    keep = [n for n in items if int(n.get("due") or 0) > now]
    if len(keep) != len(items):
        _save(keep)
    return len(keep)


def history(limit=12):
    """The last `limit` stored notifications, newest first -- READ ITEMS INCLUDED.

    The gadget's inbox shows what happened while nobody was looking. Presentation acks an item
    (so it is never re-shown unprompted), but it stays here; only :func:`cancel` and the store
    cap remove anything.
    """
    return _load()[:max(1, int(limit))]


def cancel(nid):
    """Remove one notification outright -- cancelling an un-fired reminder, or clearing a line.

    Unlike :func:`ack` (mark read), this deletes: a cancelled reminder must not still fire later.
    -> True when it was there.
    """
    items = _load()
    keep = [n for n in items if n.get("id") != nid]
    if len(keep) == len(items):
        return False
    _save(keep)
    return True


if __name__ == "__main__":   # CLI: notify.py <agent> <text> [mood] | list | clear
    if len(sys.argv) >= 2 and sys.argv[1] in ("list", "pending"):
        print(json.dumps(pending(), indent=2))
    elif len(sys.argv) >= 2 and sys.argv[1] == "all":   # include un-due reminders
        print(json.dumps([n for n in _load() if not n.get("read")], indent=2))
    elif len(sys.argv) >= 2 and sys.argv[1] == "clear":
        clear()
        print("cleared")
    elif len(sys.argv) >= 3:
        it = add(" ".join(sys.argv[2:]), agent=sys.argv[1], mood="")
        print(json.dumps(it, indent=2))
    else:
        print(__doc__)
