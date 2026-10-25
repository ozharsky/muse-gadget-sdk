"""Glance cards and conversation history for the gadget and the companion page (stdlib only).

Glance cards: short facts the gadget cycles on its idle screen and shows as rich cards on its
Glance page. Producers (glance_refresh.py,
the morning briefing, Hermes itself) POST them; each card has an id (upsert key), a kind
(weather | print | reminder | briefing | task | system | alert | note -- the gadget picks the colour),
a title, a one-line text and an optional ttl. Expired cards drop out on read.

History: the last turns from every surface (gadget voice, companion page, typed), so the
companion page shows the same conversation the gadget had.
"""
import json
import os
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GLANCE_FILE = os.path.join(HERE, "glance.json")
HISTORY_FILE = os.path.join(HERE, "history.json")
HISTORY_MAX = 80
KINDS = ("weather", "print", "reminder", "briefing", "task", "note", "system", "alert")
_lock = threading.Lock()


def _load(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def _save(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def glance_get():
    """Live cards, oldest-updated first (the gadget cycles them in this order)."""
    now = time.time()
    with _lock:
        cards = _load(GLANCE_FILE, [])
    return [c for c in cards if not c.get("until") or c["until"] > now]


def glance_set(cards, replace=False):
    """Upsert cards by id (replace=True drops every card not in this batch). Text is cut to what
    the gadget can show: title 14, text 40 characters. Returns the live set."""
    now = time.time()
    clean = []
    for c in cards or []:
        if not isinstance(c, dict) or not str(c.get("id") or "").strip():
            continue
        kind = c.get("kind") if c.get("kind") in KINDS else "note"
        # title/text: the face's 2-line caption (ASCII, 14/40 chars). value/detail/progress/icon:
        # the Glance page's rich card (big value, a longer detail line, a bar, a pixel icon).
        item = {"id": str(c["id"])[:32], "kind": kind,
                "title": str(c.get("title") or "")[:14], "text": str(c.get("text") or "")[:40],
                "label": str(c.get("label") or "")[:20], "value": str(c.get("value") or "")[:12], "detail": str(c.get("detail") or "")[:240],
                "icon": str(c.get("icon") or "")[:10], "updated": int(now)}
        # stats: up to 3 small readings side by side on the page card ({l: label, v: value, p: bar %})
        stats = []
        for st in (c.get("stats") or [])[:3]:
            if isinstance(st, dict) and st.get("v") is not None:
                one = {"l": str(st.get("l") or "")[:6], "v": str(st["v"])[:8]}
                try:
                    one["p"] = max(0, min(100, int(st["p"])))
                except (KeyError, TypeError, ValueError):
                    pass
                stats.append(one)
        if stats:
            item["stats"] = stats
        if c.get("progress") is not None:
            try:
                item["progress"] = max(0, min(100, int(c["progress"])))
            except (TypeError, ValueError):
                pass
        if c.get("ttl"):
            item["until"] = int(now + int(c["ttl"]))
        clean.append(item)
    with _lock:
        old = [] if replace else _load(GLANCE_FILE, [])
        ids = {c["id"] for c in clean}
        merged = [c for c in old if c.get("id") not in ids and (not c.get("until") or c["until"] > now)] + clean
        _save(GLANCE_FILE, merged[-12:])
    return glance_get()


def glance_delete(card_id):
    with _lock:
        cards = [c for c in _load(GLANCE_FILE, []) if c.get("id") != card_id]
        _save(GLANCE_FILE, cards)


def history_add(heard, reply, profile="default", mood="", source="gadget"):
    if not (heard or reply):
        return
    with _lock:
        rows = _load(HISTORY_FILE, [])
        rows.append({"ts": int(time.time()), "profile": profile or "default", "source": source,
                     "heard": heard or "", "reply": reply or "", "mood": mood or ""})
        _save(HISTORY_FILE, rows[-HISTORY_MAX:])


def history_get(limit=40):
    with _lock:
        return _load(HISTORY_FILE, [])[-max(1, min(limit, HISTORY_MAX)):]


if __name__ == "__main__":   # self-check on temp files
    import tempfile
    d = tempfile.mkdtemp()
    GLANCE_FILE, HISTORY_FILE = os.path.join(d, "g.json"), os.path.join(d, "h.json")
    glance_set([{"id": "w", "kind": "weather", "title": "WEATHER", "text": "52F overcast"}])
    glance_set([{"id": "p", "kind": "print", "title": "PRINTER", "text": "P2S 40%", "ttl": 1},
                {"id": "w", "kind": "bogus", "title": "x" * 30, "text": "53F"}])
    g = glance_get()
    assert [c["id"] for c in g] == ["p", "w"] and g[1]["kind"] == "note" and len(g[1]["title"]) == 14, g
    time.sleep(1.1)
    assert [c["id"] for c in glance_get()] == ["w"]
    assert [c["id"] for c in glance_set([{"id": "z", "text": "t"}], replace=True)] == ["z"]
    for i in range(100):
        history_add("q%d" % i, "a%d" % i)
    h = history_get(500)
    assert len(h) == HISTORY_MAX and h[-1]["heard"] == "q99"
    print("extras ok")
