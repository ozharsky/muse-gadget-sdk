#!/usr/bin/env python3
"""Per-profile routing config for the Muse gadget relay (stdlib only).

The relay serves a voice gadget whose firmware can target a DIFFERENT Hermes profile
per turn (``POST /turn?...&profile=<name>``, section 2 of THREE_AGENT_SPEC.md).  This
module is the ONE place that turns a profile name into the route the relay dials:

    name -> {label, voice, api_url, api_key, personality_text, ...}

Source of truth is ``agents.json`` next to this file (section 3) -- what the user edits
when they create a profile::

    {
      "default": {"label": "Hermes", "voice": "sasshole",
                  "api_key_file": "~/.hermes/config.yaml", "personality_file": null},
      "trading": {"label": "Hermes Trading", "voice": "professional",
                  "api_key_file": "~/.hermes/profiles/trading/config.yaml",
                  "personality_file": "~/.hermes/profiles/trading/SOUL.md"}
    }

* ``voice`` picks the relay's spoken PERSONA block (see relay.PERSONA_BY_VOICE:
  ``sasshole`` | ``professional`` | ``plain``).  The gadget RULES -- 1-3 sentences, the
  ``[react:]`` mood tag, the ``[image:]`` tag, the ``[task:]`` handoff and the
  approval-safety rule -- are NOT in this file and stay for every voice.
* ``api_key_file`` is the Hermes ``config.yaml`` whose ``api_server:`` ``key:`` the gateway
  authenticates the turn with.  A profile with no readable key is flagged UNUSABLE:
  :func:`resolve` then routes the turn to ``default`` instead of crashing.
* ``personality_file`` is the profile's ``SOUL.md``.  It is loaded and exposed (via
  :func:`resolve` and ``GET /agents``) so the gadget/settings can show it, but the relay
  does NOT inject it into the system prompt: the gateway already applies each profile's own
  SOUL server-side, so the relay only needs ``voice`` for the spoken tone.

Unknown or empty profile name -> ``default`` (never an error; a warning is logged to stderr).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def hermes_home():
    r"""This machine's Hermes home. HERMES_HOME wins; on Windows the install home is
    %LOCALAPPDATA%\hermes; otherwise ~/.hermes."""
    h = os.environ.get("HERMES_HOME")
    if h:
        return h
    if os.name == "nt":
        la = os.environ.get("LOCALAPPDATA")
        if la and os.path.isdir(os.path.join(la, "hermes")):
            return os.path.join(la, "hermes")
    return os.path.expanduser("~/.hermes")


def _resolve_path(path):
    r"""Expand ~, then map the portable ~/.hermes prefix onto THIS machine's Hermes home
    (macOS ~/.hermes; Windows %LOCALAPPDATA%\hermes). Separator-safe: normalizes first."""
    p = os.path.normpath(os.path.expanduser(path))
    marker = os.path.normpath(os.path.join(os.path.expanduser("~"), ".hermes"))
    if p == marker or p.startswith(marker + os.sep):
        return os.path.join(hermes_home(), p[len(marker) + 1:])
    return p
AGENTS_FILE = os.path.join(HERE, "agents.json")

DEFAULT_NAME = "default"
VALID_VOICES = ("sasshole", "professional", "plain")
# Reasoning effort a profile may pin (the relay's model_options.reasoning_effort, which the
# api_server gateway applies PER REQUEST and which beats the profile's own config value).
VALID_EFFORTS = ("none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra")
DEFAULT_VOICE = "sasshole"
_NAME_RX = re.compile(r"^[A-Za-z0-9_-]{1,32}$")   # the spec's URL-safe profile name

# The relay's own key file -- also the fallback when agents.json is absent or a profile names
# no api_key_file.  (Same target relay.read_hermes_key reads; kept here so one module owns keys.)
DEFAULT_KEY_FILE = "~/.hermes/config.yaml"

_BUILTIN_DEFAULT = {"label": "Hermes", "voice": DEFAULT_VOICE,
                    "api_key_file": DEFAULT_KEY_FILE, "personality_file": None}


def read_api_key_file(path):
    """The gateway API key inside a Hermes ``config.yaml``, or None.

    Scans the ``api_server:`` block by indentation -- a superset of relay's old rigid regex
    (``api_server: / enabled: true / extra: / key:``).  That regex missed profiles that write
    the sub-keys in a different order: ``marketing`` has ``extra:`` BEFORE ``enabled:``, and
    the live gateway DOES authenticate its key, so a key present must never be read as "absent".
    Returns None on a missing file or an ``api_server`` block with no non-empty ``key:``.
    """
    try:
        lines = open(_resolve_path(path)).read().splitlines()
    except OSError:
        return None
    for i, line in enumerate(lines):
        if not re.match(r"^\s*api_server:\s*$", line):
            continue
        base = len(line) - len(line.lstrip())
        for nxt in lines[i + 1:]:
            if nxt.strip() and (len(nxt) - len(nxt.lstrip())) <= base:
                break                                  # dedented out of the api_server block
            m = re.match(r"\s*key:\s*(\S+)", nxt)
            if m:
                return m.group(1).strip("\"'") or None
    return None


def read_personality_file(path):
    """The profile's SOUL.md text (trimmed), or None if unset/missing. Informational only: the
    gateway applies each profile's own SOUL server-side; the relay's spoken tone uses ``voice``."""
    if not path:
        return None
    try:
        return open(_resolve_path(path)).read().strip()
    except OSError:
        return None


def _entry(name, raw, default_voice):
    """Validate one agents.json entry; never raises.  ``usable`` is False when the resolved key
    file yields no key, so :func:`resolve` can route that profile's turns to ``default``."""
    raw = raw if isinstance(raw, dict) else {}
    key_file = raw.get("api_key_file") or (DEFAULT_KEY_FILE if name == DEFAULT_NAME else None)
    voice = str(raw.get("voice") or "").strip().lower()
    if voice not in VALID_VOICES:
        voice = default_voice
    effort = str(raw.get("effort") or raw.get("reasoning_effort") or "").strip().lower()
    if effort not in VALID_EFFORTS:
        effort = ""
    key = read_api_key_file(key_file) if key_file else None
    reason = "" if key else ("no api_key_file" if not key_file
                             else "no api_server key in %s" % key_file)
    return {"name": name, "label": str(raw.get("label") or name), "voice": voice,
            "voice_name": str(raw.get("voice_name") or "").strip(),
            "effort": effort, "api_key_file": key_file, "personality_file": raw.get("personality_file"),
            "personality_text": read_personality_file(raw.get("personality_file")),
            "api_key": key, "usable": bool(key), "reason": reason}


def load(path=None):
    """-> ``{name: entry}``.  Always contains ``default``.  A missing / unreadable / malformed
    agents.json degrades to the built-in default entry (plus whatever names do parse) -- never
    an exception, so a half-written file can't take the relay's turns down."""
    path = path or AGENTS_FILE
    try:
        data = json.load(open(_resolve_path(path)))
        if not isinstance(data, dict):
            raise ValueError("agents.json is not a JSON object")
    except (OSError, ValueError) as e:
        print("agents.json unusable (%s); using the built-in default profile" % e,
              file=sys.stderr, flush=True)
        data = {}
    raw_default = data.get(DEFAULT_NAME)
    raw_default = raw_default if isinstance(raw_default, dict) else _BUILTIN_DEFAULT
    dv = str(raw_default.get("voice") or "").strip().lower()
    default_voice = dv if dv in VALID_VOICES else DEFAULT_VOICE
    out = {}
    for name in [DEFAULT_NAME] + [n for n in data if n != DEFAULT_NAME]:
        if not isinstance(name, str) or not _NAME_RX.match(name):
            print("agents.json: skipping bad profile name %r" % (name,), file=sys.stderr, flush=True)
            continue
        fallback_raw = _BUILTIN_DEFAULT if name == DEFAULT_NAME else {}
        out[name] = _entry(name, data.get(name, fallback_raw), default_voice)
    return out


def resolve(name, base=None, path=None):
    """The turn's target ``name`` -> the route to dial:

        {name, requested, label, voice, api_url, api_key, personality_text, usable, fallback}

    * empty / ``default`` / a malformed name  -> the default route.
    * an UNKNOWN name, or a known profile whose key cannot be read -> the default route with
      ``fallback=True`` and a warning on stderr (the turn must never error over config).
    * ``api_url`` is ``{base}/v1/chat/completions`` for default and
      ``{base}/p/<name>/v1/chat/completions`` otherwise.
    """
    base = (base or os.environ.get("HERMES_URL", "http://127.0.0.1:8642")).rstrip("/")
    entries = load(path)
    default = entries[DEFAULT_NAME]
    raw = (name or "").strip()
    if raw in ("", DEFAULT_NAME) or not _NAME_RX.match(raw):
        if raw and raw != DEFAULT_NAME and not _NAME_RX.match(raw):
            print("ignoring invalid profile name %r: routing to default" % raw, file=sys.stderr, flush=True)
        requested = ""
    else:
        requested = raw
    chosen, fallback = default, False
    if requested:
        ent = entries.get(requested)
        if ent is None:
            print("unknown profile %r: routing to default" % requested, file=sys.stderr, flush=True)
            fallback = True
        elif not ent["usable"]:
            print("profile %r unusable (%s): routing to default" % (requested, ent["reason"]),
                  file=sys.stderr, flush=True)
            fallback = True
        else:
            chosen = ent
    used = chosen["name"]
    url = base + ("/v1/chat/completions" if used == DEFAULT_NAME
                  else "/p/%s/v1/chat/completions" % used)
    return {"name": used, "requested": requested, "label": chosen["label"], "voice": chosen["voice"],
            "voice_name": chosen["voice_name"], "effort": chosen["effort"],
            "api_url": url, "api_key": chosen["api_key"] or "",
            "personality_text": chosen["personality_text"],
            "usable": chosen["usable"], "fallback": fallback}


def describe(path=None):
    """Serializable profile list for ``GET /agents``: names + labels + voice + usability,
    re-read each call, never any key material."""
    return [{"name": e["name"], "label": e["label"], "voice": e["voice"],
             "voice_name": e["voice_name"], "effort": e["effort"],
             "usable": e["usable"], "reason": e["reason"]}
            for e in load(path).values()]
