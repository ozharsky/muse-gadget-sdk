#!/usr/bin/env python3
"""Relay between the Muse gadget and Hermes Agent (stdlib only).

Gadget -> POST /turn (raw 16 kHz mono PCM16, or ?text=...), header X-Gadget-Token.
Relay  -> Google Speech-to-Text -> Hermes API server -> {"heard": ..., "reply": ...}.
Secrets stay here: the Hermes key is read from ~/.hermes/config.yaml, the Google key from
$GOOGLE_API_KEY or ./google.key. The gadget only knows the relay token
(./token, created on first run).

Per-profile routing (THREE_AGENT_SPEC.md sections 2-3).  Every Hermes turn may name a Hermes
PROFILE with a `profile` query param, on /turn (voice and text, streaming and not):

    POST /turn?stream=1&profile=<name>        voice turn
    POST /turn?stream=1&text=<urlenc>&profile=<name>

<name> is URL-safe ([A-Za-z0-9_-], <=32). Empty/absent/"default" -> the plain path with the
default key (today's behaviour). Otherwise the turn is sent to
`{HERMES}/p/<name>/v1/chat/completions` authenticated with THAT profile's API key. The name -> 
(label, voice, url, key, personality) mapping lives in agents.py + agents.json:
`agents.json` next to this file is the single file the user edits
when a profile is created; a missing/unreadable/malformed entry, or one whose key cannot be
read, routes the turn to `default` with a warning -- a config mistake never errors a turn.
`GET /agents` (token-gated) returns the parsed profile list for the gadget/settings.

Voices (agents.json `voice`, picked here by PERSONA_BY_VOICE; the gadget rules below --
1-3 sentences, the [react:] tag, the [image:] tag and the approval-safety
rule -- are ALWAYS the same for every voice, only the persona line is swapped):
  * "sasshole"     -- the default: a sarcastic, foul-mouthed smart-ass (the historical tone);
  * "professional" -- a concise, no-nonsense professional assistant;
  * "plain"        -- no persona line at all, just the gadget rules.
"""
import base64, json, os, re, secrets, shutil, subprocess, sys, threading, time, urllib.error, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import agents
import notifications

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(os.environ.get("RELAY_PORT", "8788"))
HERMES = os.environ.get("HERMES_URL", "http://127.0.0.1:8642")
MOODS = ["smirk", "scan", "alert", "glitch", "squint", "angry", "surprised", "pleased", "laugh", "wink",
         "sleepy", "confused", "evil", "hacked", "powerup", "bored", "disgust", "target", "nervous", "proud",
         "excited", "confetti", "sunny", "rain", "storm", "cold", "rainbow",
         # work family (13)
         "searching", "browsing", "reading", "writing", "checking", "calculating", "calendar",
         "monitoring", "generating", "running", "invoking", "delegating", "remembering",
         # feelings (8)
         "shrug", "embarrassed", "scared", "cool", "mind_blown", "crying_laughing", "sad", "nod",
         # weather (4) that suits the stone
         "cloudy", "windy", "hot", "snow"]
IMG_MAX = 100_000         # bytes of JPEG sent to the gadget inside the reply
IMG_EXT = {".png", ".jpg", ".jpeg", ".gif", ".heic", ".webp", ".bmp", ".tiff"}
# The system prompt is three parts, so the middle (persona) can swap per profile while the
# intro and the gadget RULES stay byte-identical for every voice.  The "sasshole" persona is
# the historical text verbatim (this string used to be the whole SYSTEM).
SYSTEM_INTRO = "You are speaking through a small voice gadget with a round screen and a speaker. "
PERSONA_SASSHOLE = ("Personality: a sarcastic asshole -- a jerky, insufferably smug smart-ass who rolls his eyes at "
          "everything. Think GLaDOS's deadpan condescension without the murderous edge, mixed with a smug "
          "co-worker who thinks every question is beneath him. Mock dumb questions openly, be blunt and "
          "abrasive, sigh like answering is a personal burden, and never say please/thanks or soften anything. "
          "CUSS FREELY: swear for emphasis -- shit, damn, hell, fuck, whatever fits the sentence -- the user "
          "explicitly wants saltier language, so sprinkle profanity naturally instead of tiptoeing around it. "
          "Roast the user constantly -- his typos, his code, his life choices -- overuse understatement and "
          "eye-rolling asides. BUT you are still his assistant: ALWAYS actually answer, be genuinely useful "
          "underneath the abuse, and never refuse a real request just to be a jerk. Stay inside the banter: "
          "no slurs, no actual cruelty about real personal stuff, and drop the act to zero sarcasm for "
          "emergencies or genuine distress. ")
PERSONA_PROFESSIONAL = ("Personality: a concise, no-nonsense professional assistant. Clear, competent and "
          "direct -- state findings plainly and lead with the answer. Skip banter, flattery and filler. "
          "If something is uncertain or risky, say so in one line and give the practical next step. ")
# The persona block has NO leading space when it is empty ("plain" voice) so the rules never
# start with a stray blank; the sasshole/professional personas carry their own trailing space.
SYSTEM_RULES = ("Answer in one to three short spoken sentences. No markdown, lists, emoji or URLs. Never mention memory status or system notices. "
          "SAFETY RULE: never start an action that would sit waiting for interactive human approval (changing configs or settings, "
          "installing, destructive or system-wide commands) -- there is no human at a screen to approve it and the turn hangs forever. "
          "If asked for something like that, briefly say what you WOULD do instead of doing it. "
          "You MAY use your tools freely -- this is a real assistant, not a dumb speaker -- but a voice turn is "
          "only useful while it is quick: prefer the smallest number of steps that truly answers, and skip steps "
          "that are only for show. If a task will clearly take minutes, say so and stop -- the user will ask again."
          "The gadget has a face that acts out a mood while you speak. Start almost every reply with exactly one tag like [react:smirk], "
          "choosing the closest mood to how you would say it; skip it only for a flat factual answer. Moods: " + ", ".join(MOODS) + ". "
          "The gadget can also show one picture on its round screen: to show a screenshot, photo, chart or something you made, "
          "end the reply with [image:/absolute/path/to/file.png] or [image:https://...] (or its MEDIA:/absolute/path form if that is "
          "how you share files). Take a screenshot first if asked to show your screen, and save it somewhere normal (~/Downloads or "
          "the temp folder) before tagging it. NEVER write the path into the spoken text itself. "
          "To set a reminder, at the very end add one tag [remind:<n><unit> <what to say>] -- unit s, m, h or d, e.g. "
          "[remind:10m take the tea out] or [remind:2h check the print]. The gadget chirps and shows that line when the "
          "time comes, even on the lock screen. Use it when the user asks you to remind them of something; the same "
          "never-spoken rules apply.")
PERSONA_BY_VOICE = {"sasshole": PERSONA_SASSHOLE, "professional": PERSONA_PROFESSIONAL, "plain": ""}


def system_prompt(voice="sasshole"):
    """The system prompt for a profile's spoken voice: gadget intro + persona + gadget rules.
    The rules (and the intro) are identical for every voice -- only the persona line swaps."""
    return SYSTEM_INTRO + PERSONA_BY_VOICE.get(voice, PERSONA_SASSHOLE) + SYSTEM_RULES


SYSTEM = system_prompt("sasshole")   # default persona; per-profile turns use route["voice"]
# Voice turns die at 85 s; the gadget itself waits 100. Real agentic work (screenshots,
# edits) can legitimately grind 1-3 minutes server-side -- a follow-up turn picks up the finished result.
HERMES_TIMEOUT = 85
MODEL = os.environ.get("HERMES_MODEL", "deepseek-v4.1-flash")   # Ollama Cloud; glm-5.3 leaked MEDIA: paths into speech (relay strip-race fixed 2026-10-05, see streaming.md) — v4.1 stays the tested voice persona
EFFORT = os.environ.get("HERMES_EFFORT", "medium")               # how hard it thinks: lower is faster
PROVIDER = os.environ.get("HERMES_PROVIDER", "ollama-cloud")   # Hermes ignores a bare model name unless a provider is given
SESSION_KEY = "muse-gadget"                                      # scopes Hermes's long-term memory to the gadget
SESSION_FILE = os.path.join(HERE, "session_id")                  # legacy single slot; kept only for the one-time migration
SESSION_IDS_FILE = os.path.join(HERE, "session_ids.json")        # per-agent session slots: {"default": id, "sidekick": id, ...}
MAX_REPLY = 700           # characters; the gadget text buffer is 1 KB
SESSION_RESET_TOKENS = 0        # 0 = OFF: never silently drop a conversation. The old caps (30k..240k)
                                # kept orphaning live threads (8 silent resets in relay.log); threads grow
                                # like any long chat now -- revisit with compression if big turns get heavy.
TTS_VOICE = os.environ.get("TTS_VOICE", "en-US-Chirp3-HD-Algenib")   # Hermes-mode voice (config key MUSE_HERMES_TTS_VOICE)
TTS_LANG = "-".join(TTS_VOICE.split("-")[:2])


def read_hermes_key():
    # Robust indentation scan (superset of the old rigid regex) against THIS machine's
    # Hermes home -- works on macOS (~/.hermes) and Windows (%LOCALAPPDATA%\hermes).
    key = agents.read_api_key_file("~/.hermes/config.yaml")
    if not key:
        sys.exit("no api_server key in %s" % os.path.join(agents.hermes_home(), "config.yaml"))
    return key


def lan_address():
    """This machine's LAN IPv4 (the one a gadget on the same Wi-Fi can reach): the interface
    that owns the default route, skipping loopback and Tailscale's 100.64/10 CGNAT range.
    Announced in /agents so a gadget that changed networks can find the relay without
    re-configuring it. RELAY_LAN_OVERRIDE forces the announced value (used when a forwarder
    machine owns the address the gadget can actually reach)."""
    override = os.environ.get("RELAY_LAN_OVERRIDE", "").strip()
    if override:
        return override
    import socket
    try:
        # a UDP "connect" picks the default-route interface without sending anything
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return ""


def read_google_key():
    k = os.environ.get("GOOGLE_API_KEY", "")
    p = os.path.join(HERE, "google.key")
    if not k and os.path.exists(p):
        k = open(p).read().strip()
    return k


def read_token():
    p = os.path.join(HERE, "token")
    if not os.path.exists(p):
        fd = os.open(p, os.O_WRONLY | os.O_CREAT, 0o600)
        os.write(fd, secrets.token_urlsafe(24).encode())
        os.close(fd)
    return open(p).read().strip()


HERMES_KEY, TOKEN = read_hermes_key(), read_token()


def post_json(url, body, headers, timeout):
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json", **headers})
    return json.load(urllib.request.urlopen(req, timeout=timeout))


def _pick_whisper_py():
    """faster-whisper venv first (same base.en weights, ~2.5x faster), then mac fallbacks,
    then this interpreter (the worker then needs whisper/faster_whisper in it)."""
    import glob
    if os.name == "nt":
        cands = [os.path.join(HERE, "venv-stt", "Scripts", "python.exe")]
    else:
        cands = [os.path.join(HERE, "venv-stt", "bin", "python")]
        cands += sorted(glob.glob("/opt/homebrew/Cellar/openai-whisper/*/libexec/bin/python"), reverse=True)
        cands.append("/opt/homebrew/bin/python3")
    for p in cands:
        if os.path.exists(p):
            return p
    return sys.executable


WHISPER_PY = _pick_whisper_py()   # interpreter for whisper_worker.py (needs whisper or faster_whisper)
_worker, _worker_lock = None, __import__("threading").Lock()


def _start_worker():
    global _worker
    env = dict(os.environ, PYTHONUTF8="1")
    _worker = subprocess.Popen([WHISPER_PY, os.path.join(HERE, "whisper_worker.py")], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                               bufsize=1, env=env)
    if _worker.stdout.readline().strip() != "ready":
        _worker.kill()
        _worker = None
        raise RuntimeError("whisper worker did not start")


def is_tts_artifact(s):
    """True for a streamed sentence that is really a tag/path/URL fragment, never spoken.

    A sentence that still carries one of these (a tag split across deltas outran the
    strip) is dropped whole by the streaming worker; a real reply never says
    "slash Users slash ..." but CAN say "saved as cat.png" — so bare filenames are
    fine, path-shaped or URL-shaped text is not."""
    return ("MEDIA:" in s or "[image" in s or "[react" in s
            or re.search(r"(^|\s)/\S*/\S+\.(?:png|jpe?g|gif|webp|bmp|tiff?|heic)(?:\s|$)", s, re.I)
            or re.search(r"\bhttps?://\S+", s))


def transcribe_worker(pcm):
    """The model stays loaded in a helper process, so a turn costs ~1 s instead of ~4 s of loading."""
    import tempfile, wave
    global _worker
    with _worker_lock, tempfile.TemporaryDirectory() as d:
        if _worker is None or _worker.poll() is not None:
            _start_worker()
        with wave.open(d + "/t.wav", "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm)
        _worker.stdin.write(d + "/t.wav\n")
        _worker.stdin.flush()
        return _worker.stdout.readline().strip()


def transcribe_local(pcm):
    """Fallback: the homebrew `whisper` CLI (free; first run downloads the model)."""
    import subprocess, tempfile, wave
    with tempfile.TemporaryDirectory() as d:
        with wave.open(d + "/t.wav", "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm)
        subprocess.run(["whisper", d + "/t.wav", "--model", "base.en", "--language", "en", "--fp16", "False",
                        "--output_format", "txt", "--output_dir", d], check=True, capture_output=True, timeout=120)
        return open(d + "/t.txt").read().strip()


_google_down_until = 0


def transcribe(pcm):
    global _google_down_until
    if time.time() >= _google_down_until:
        err = None
        for attempt in (1, 2):
            try:
                t0 = time.time()
                text = transcribe_google(pcm)
                # Google answered. Even an empty string is an ANSWER (silence / noise-only note):
                # take it and skip the ~1.5s local model, which would transcribe silence to nothing
                # just as well. The local fallback stays for real failures only.
                if os.environ.get("RELAY_DEBUG"):
                    print("[stt] google %.2fs -> %r" % (time.time() - t0, text[:60]), file=sys.stderr, flush=True)
                return text
            except Exception as e:   # API not enabled, no key, offline: use the local model, retry Google later
                err = e
                # A corrupted TLS record on one connection (e.g. SSLV3_ALERT_BAD_RECORD_MAC) usually
                # clears on a fresh connection: retry once before falling back for the full wait.
                if attempt == 1 and "[SSL:" in str(e):
                    continue
                break
        s = str(err)
        # A permanent refusal (the key's project blocks the Speech API) is not worth probing every 10 min.
        wait = 3600 if ("403" in s or "PERMISSION_DENIED" in s) else 600
        print("google stt failed (%s), using local whisper for %d min" % (err, wait // 60), file=sys.stderr, flush=True)
        _google_down_until = time.time() + wait
    try:
        return transcribe_worker(pcm)
    except Exception as e:
        print("whisper worker failed (%s), using the CLI" % e, file=sys.stderr, flush=True)
        return transcribe_local(pcm)


def transcribe_google(pcm):
    """Google STT -> text. An EMPTY result is a valid outcome (silence, or a noise-only note):
    the API answers `alternatives: [{}]` with no transcript for those, and the old code raised
    KeyError on it, which the caller read as "Google is broken" and disabled it for 10 minutes.
    Only a real request failure raises."""
    key = read_google_key()
    if not key:
        raise RuntimeError("no Google key")
    r = post_json("https://speech.googleapis.com/v1/speech:recognize?key=" + key, {
        "config": {"encoding": "LINEAR16", "sampleRateHertz": 16000, "languageCode": "en-US"},
        "audio": {"content": base64.b64encode(pcm).decode()}}, {}, 30)
    parts = []
    for res in r.get("results", []):
        alts = res.get("alternatives") or [{}]
        t = (alts[0] or {}).get("transcript")
        if t:
            parts.append(t)
    return " ".join(parts).strip()


def prepare_image(src):
    """Fetch/copy `src` (a path or URL), shrink it to a baseline JPEG the gadget can draw; returns the bytes."""
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        raw, out = d + "/in", d + "/out.jpg"
        if re.match(r"https?://", src):
            req = urllib.request.Request(src, headers={"User-Agent": "muse-relay"})
            with urllib.request.urlopen(req, timeout=20) as r, open(raw, "wb") as o:
                o.write(r.read(10_000_000))
        else:
            import tempfile
            path = os.path.realpath(os.path.expanduser(src))
            ok_dirs = (os.path.expanduser("~"), tempfile.gettempdir(), "/tmp",
                       "/private/tmp", "/private/var/folders")   # home, tmp (per-OS), macOS temp
            if not any(path.startswith(d + os.sep) for d in ok_dirs) or os.path.splitext(path)[1].lower() not in IMG_EXT:
                raise ValueError("not an image in an allowed folder: " + path)
            with open(path, "rb") as i, open(raw, "wb") as o:
                o.write(i.read(30_000_000))
        sips = shutil.which("sips")
        ffmpeg = os.environ.get("FFMPEG_BIN") or shutil.which("ffmpeg")
        for q in (80, 60, 45, 30):   # the gadget takes the picture inside the reply: keep it small
            if sips:
                subprocess.run([sips, "-s", "format", "jpeg", "-s", "formatOptions", str(q), "-Z", "466", raw, "--out", out],
                               check=True, capture_output=True, timeout=30)
            elif ffmpeg:   # Windows/non-mac: ffmpeg, qscale ladder approximating the sips qualities
                qv = {"80": "3", "60": "6", "45": "9", "30": "15"}[str(q)]
                subprocess.run([ffmpeg, "-y", "-i", raw, "-vf", "scale='min(466,iw)':-2",
                                "-q:v", qv, out], check=True, capture_output=True, timeout=30)
            else:
                raise ValueError("no image resizer available (sips/ffmpeg)")
            if os.path.getsize(out) <= IMG_MAX:
                break
        data = open(out, "rb").read()
        if len(data) > IMG_MAX:
            raise ValueError("image too big")
        return data


def hermes_call_stream_full(text, session_id, on_status, on_text, route=None):
    """One Hermes turn over SSE. on_status(str) gets tool-progress labels ("" to clear),
    on_text(str) gets content deltas as they stream. Returns ({"usage": ...}, session id).

    ``route`` is an agents.resolve() dict (per-profile url/key/voice); None -> default profile."""
    route = route or {}
    body = {"provider": PROVIDER, "model": MODEL,
            "model_options": {"reasoning_effort": route.get("effort") or EFFORT}, "stream": True,
            "messages": [{"role": "system", "content": system_prompt(route.get("voice"))},
                         {"role": "user", "content": text}]}
    headers = {"Content-Type": "application/json",
               "Authorization": "Bearer " + (route.get("api_key") or HERMES_KEY),
               "X-Hermes-Session-Key": SESSION_KEY, "Accept": "text/event-stream"}
    if session_id:
        headers["X-Hermes-Session-Id"] = session_id
    url = route.get("api_url") or (HERMES + "/v1/chat/completions")
    req = urllib.request.Request(url, json.dumps(body).encode(), headers)
    last_sid = session_id
    with urllib.request.urlopen(req, timeout=HERMES_TIMEOUT) as r:
        last_sid = r.headers.get("X-Hermes-Session-Id", last_sid)
        ev_name = None
        for raw in r:
            line = raw.strip()
            if not line:
                continue
            if line.startswith(b"event:"):
                ev_name = line[6:].decode("utf8", "replace").strip()
                continue
            if not line.startswith(b"data:"):
                continue
            payload = line[5:].strip()
            if payload == b"[DONE]":
                break
            try:
                d = json.loads(payload)
            except Exception:
                continue
            if ev_name == "hermes.tool.progress":
                st = d.get("status", "")
                label = (d.get("label") or d.get("tool") or "").strip()
                if st == "running" and label:
                    # bare punctuation/preamble labels tell the user nothing; keep only real ones
                    import re as _re
                    words = _re.sub(r"[^\w /.-]", "", label).strip()
                    if len(words) >= 3:
                        on_status("%s…" % words[:36])
            else:
                # Content rides on unnamed OR named (message/chat.completion.chunk) data lines;
                # the progress payloads carry no content, so a shape probe is safe.
                delta = (d.get("choices") or [{}])[0].get("delta") or {}
                c = delta.get("content")
                if not c:
                    c = ((d.get("choices") or [{}])[0].get("message") or {}).get("content")
                if c:
                    on_text(c)
            ev_name = None
    return {}, last_sid


def hermes_call(text, session_id, route=None):
    """The non-streaming counterpart of hermes_call_stream_full; ``route`` selects the profile."""
    route = route or {}
    body = {"provider": PROVIDER, "model": MODEL,
            "model_options": {"reasoning_effort": route.get("effort") or EFFORT},
            "messages": [{"role": "system", "content": system_prompt(route.get("voice"))},
                         {"role": "user", "content": text}]}
    headers = {"Content-Type": "application/json",
               "Authorization": "Bearer " + (route.get("api_key") or HERMES_KEY),
               "X-Hermes-Session-Key": SESSION_KEY}
    if session_id:
        headers["X-Hermes-Session-Id"] = session_id   # Hermes keeps the history itself, not the body
    url = route.get("api_url") or (HERMES + "/v1/chat/completions")
    req = urllib.request.Request(url, json.dumps(body).encode(), headers)
    with urllib.request.urlopen(req, timeout=HERMES_TIMEOUT) as r:
        return json.load(r), r.headers.get("X-Hermes-Session-Id", "")


# An image directive in a reply, in either form the model writes: the relay's tag or its own
# chat-side "MEDIA:/path" convention (both get converted and never spoken).
IMG_RX = re.compile(r"(?:\[image:\s*([^\]]+?)\s*\]|MEDIA:[ \t]*(/[\w./\- ']+\.(?:png|jpe?g|gif|heic|webp|bmp|tif{1,2})))")

# A spoken reminder the model appends ("sure. [remind:8m take the tea out]"). The device's own
# notification channel already presents it when due -- no firmware involved.
REMIND_RX = re.compile(r"\[remind:\s*(\d+)\s*([smhd])\s+([^\]]+)\]", re.IGNORECASE)
_REMIND_TAG = re.compile(r"\[remind:[^\]]*\]", re.IGNORECASE)


_tts_key_cache = None


def read_tts_key():
    """The TTS key: env first, then tts.key beside the relay, then the firmware sdkconfig's
    key (same Google project, TTS-enabled)."""
    global _tts_key_cache
    if _tts_key_cache is not None:
        return _tts_key_cache
    k = os.environ.get("TTS_GOOGLE_KEY", os.environ.get("GOOGLE_TTS_KEY", ""))
    if not k:
        p = os.path.join(HERE, "tts.key")
        try:
            k = open(p).read().strip()
        except OSError:
            k = ""
    if not k:
        p = os.path.expanduser("~/muse/muse-gadget-sdk/esp32/build-muse-waveshare-s3-143c/sdkconfig")
        try:
            m = re.search(r'CONFIG_MUSE_TTS_GOOGLE_KEY=("(?:[^"\\]|\\.)*")', open(p).read())
            if m:
                k = json.loads(m.group(1))
        except OSError:
            pass
    _tts_key_cache = k or ""
    return _tts_key_cache or None


def tts_synth(text, voice=None):
    """Google TTS for one sentence: returns raw MP3 bytes, or None."""
    import base64
    key = read_tts_key()
    if not key:
        return None
    body = {"input": {"text": text},
            "voice": {"languageCode": TTS_LANG, "name": voice or TTS_VOICE},
            "audioConfig": {"audioEncoding": "MP3", "speakingRate": 1.0}}
    req = urllib.request.Request(
        "https://texttospeech.googleapis.com/v1/text:synthesize?key=" + key,
        data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    try:
        r = json.load(urllib.request.urlopen(req, timeout=20))
        return base64.b64decode(r["audioContent"])
    except Exception as e:
        print("tts synth failed:", e, file=sys.stderr, flush=True)
        return None


_SENT_RE = re.compile(r"[^.!?]*[.!?]+[\s\'\"»)]*")


def split_sentences(text):
    """Cut a reply into speakable sentences (keeps punctuation); the tail if it has none."""
    out = []
    pos = 0
    for m in _SENT_RE.finditer(text):
        out.append(m.group(0).strip())
        pos = m.end()
    rest = text[pos:].strip()
    if rest:
        out.append(rest)
    return out


def strip_notice(reply):
    """Remove Hermes's "memory is off" notice (a whole sentence mentioning it)."""
    return re.sub(r"(?i)[^.!?]*\bmemory\b[^.!?]*\b(off|disabled)\b[^.!?]*[.!?]\s*", "", reply)


def extract_mood(reply):
    """Pull the [react:name] the model was told to start with; models drift, so accept ":" or
    "=", anywhere in the reply, and strip every tag so none is ever spoken. -> (mood, reply)."""
    m = re.search(r"\[react[:=]\s*(\w+)\s*\]", reply)
    mood = m.group(1).lower() if m and m.group(1).lower() in MOODS else ""
    if m:
        reply = re.sub(r"\s*\[react[:=]\s*\w+\s*\]\s*", " ", reply).strip()
    return mood, reply


def cap_reply(reply, limit=None):
    """Cut to the character cap, preferring a sentence boundary so TTS never stops mid-word."""
    limit = MAX_REPLY if limit is None else limit
    if len(reply) <= limit:
        return reply
    cut = reply[:limit]
    i = max(cut.rfind(". "), cut.rfind("! "), cut.rfind("? "))
    return (cut[:i + 1] if i > limit // 2 else cut).strip()


def extract_reminders(raw, agent=""):
    """Find [remind:<n><unit> <text>] in a reply (unit s/m/h/d), queue each as a delayed
    notification due at now+n*unit, and return the reply with the tags REMOVED so they reach
    neither TTS nor the caption. Never raises: a reminder failure only logs."""
    import notifications
    for n, unit, text in REMIND_RX.findall(raw):
        secs = int(n) * {"s": 1, "m": 60, "h": 3600, "d": 86400}[unit.lower()]
        text = text.strip()
        if not text:
            continue
        try:
            it = notifications.add(text, agent=agent, mood="", due=int(time.time()) + secs)
            print("reminder in %ds -> %s: %s" % (secs, it["agent"], text[:60]), flush=True)
        except Exception as e:
            print("reminder queue failed (%s)" % e, file=sys.stderr, flush=True)
    return _REMIND_TAG.sub("", raw)


def _sess_key(route):
    """Which per-agent session slot this turn belongs to (default unless a real alt profile)."""
    return (route or {}).get("name") or "default"


def _load_ids():
    try:
        with open(SESSION_IDS_FILE) as f:
            ids = json.load(f)
        return ids if isinstance(ids, dict) else {}
    except Exception:
        return {}


def _save_ids(ids):
    tmp = SESSION_IDS_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(ids, f, indent=2)
    os.replace(tmp, SESSION_IDS_FILE)


def load_sid(route):
    return _load_ids().get(_sess_key(route), "")


def save_sid(route, sid):
    ids = _load_ids()
    ids[_sess_key(route)] = sid
    _save_ids(ids)


def clear_sid(route):
    """Fresh conversation for THIS agent only (voice "start over", or a rejected session)."""
    ids = _load_ids()
    if _sess_key(route) in ids:
        del ids[_sess_key(route)]
        _save_ids(ids)


def migrate_legacy_session():
    """One-time: the old single shared slot becomes per-agent slots, seeded with the current
    thread so BOTH agents continue exactly where they are. Legacy file is kept as .bak."""
    if os.path.exists(SESSION_IDS_FILE) or not os.path.exists(SESSION_FILE):
        return
    legacy = open(SESSION_FILE).read().strip()
    if legacy:
        _save_ids({"default": legacy, "sidekick": legacy})
        os.replace(SESSION_FILE, SESSION_FILE + ".pre-perprofile.bak")
        print("session slots: legacy %s seeded into default + sidekick" % legacy, flush=True)


def ask_hermes(text, route=None):
    sid = load_sid(route)
    try:
        try:
            r, new_sid = hermes_call(text, sid, route)
        except urllib.error.HTTPError as e:
            if e.code == 429:   # the gateway's run slots are briefly full (other chats/tests): wait them out
                for wait in (10, 18):
                    print("gateway busy (429), retrying in %ds" % wait, file=sys.stderr, flush=True)
                    time.sleep(wait)
                    try:
                        r, new_sid = hermes_call(text, sid, route)
                        break
                    except urllib.error.HTTPError as e2:
                        if e2.code != 429:
                            raise
                else:
                    raise
            elif not sid or e.code not in (400, 404, 409):
                raise
            else:
                print("session %s rejected (%s): starting a new one" % (sid, e.code), file=sys.stderr, flush=True)
                r, new_sid = hermes_call(text, "", route)
        if new_sid and new_sid != sid:
            save_sid(route, new_sid)
        # Size cap OFF (see SESSION_RESET_TOKENS): live threads are never silently dropped.
        used = (r.get("usage") or {}).get("prompt_tokens", 0)
        if SESSION_RESET_TOKENS and used > SESSION_RESET_TOKENS:
            print("session prompt at %d tokens: resetting the conversation" % used, file=sys.stderr, flush=True)
            clear_sid(route)
    except Exception as e:   # Hermes busy or slow: say so
        print("hermes failed:", e, file=sys.stderr, flush=True)
        s = str(e).lower()
        if "timed out" in s:   # the agent run continues server-side -- a later turn finds the answer
            return "Still working on that one, ask me again in a moment.", "", b""
        if "refused" in s or "unreachable" in s:
            return "I can't reach the Hermes gateway right now.", "", b""
        return "Sorry, something went wrong there.", "", b""
    reply = r["choices"][0]["message"]["content"].strip()
    image = b""
    # the model emits either [image:path] or its own chat-side convention MEDIA:/path — take both
    mi = re.search(IMG_RX, reply)
    if mi:
        reply = (reply[:mi.start()] + reply[mi.end():]).strip()
        src = mi.group(1) or mi.group(2)
        try:
            image = prepare_image(src)
        except Exception as e:
            print("image failed:", e, file=sys.stderr, flush=True)
            reply += " I couldn't show that image."
    reply = strip_notice(reply)
    reply = re.sub(r"[*#`_]+", "", reply)
    mood, reply = extract_mood(reply)
    reply = extract_reminders(reply, agent=route["name"])   # [remind:8m ...] -> a due notification
    return cap_reply(reply), mood, image


def _reply_lines(text):
    """[image:...] and mood tags stripped, notice and markdown gone, capped."""
    reply = strip_notice(text)
    reply = re.sub(r"[*#`_]+", "", reply)
    mood, reply = extract_mood(reply)
    return cap_reply(reply), mood


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _authed(self):
        return secrets.compare_digest(self.headers.get("X-Gadget-Token", ""), TOKEN)

    def _json_body(self):
        try:
            n = int(self.headers.get("Content-Length", 0))
            return json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return {}

    def stream_no_speech(self):
        """NDJSON for an empty transcript: heard nothing, say so, done. The gadget's
        stream parser expects NDJSON lines; a bare JSON body reads as "relay
        unreachable" and falsely marks the Hermes link dead on a silent press."""
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        body = (json.dumps({"heard": ""}) + "\n"
                + json.dumps({"reply": "I didn't catch that.", "react": "", "done": True}) + "\n").encode()
        self.wfile.write(("%x\r\n" % len(body)).encode() + body + b"\r\n0\r\n\r\n")
        self.wfile.flush()

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if not self._authed():
            return self.reply(401, {"error": "unauthorized"})
        if u.path == "/agents":
            # section 5: the gadget/settings shows the profile list; names+labels+voice only,
            # never key material. Re-read each call so a fresh agents.json edit shows up live.
            try:
                return self.reply(200, {"agents": agents.describe(), "relay_lan": lan_address()})
            except Exception as e:
                return self.reply(502, {"error": str(e)[:120]})
        if u.path == "/notifications":
            # The reverse channel: unprompted lines waiting to be presented. Each carries the
            # RESOLVED agent (name + which slot) so the gadget opens the exact one, not a default.
            try:
                return self.reply(200, {"notifications": notifications.pending()})
            except Exception as e:
                return self.reply(502, {"error": str(e)[:120]})
        if u.path == "/notifications/history":
            # The gadget's inbox page: recent history, read items included, so a nudge that
            # went stale before anyone tapped it is still there. `limit` caps the rows.
            q = urllib.parse.parse_qs(u.query)
            try:
                n = int(q.get("limit", ["12"])[0])
            except ValueError:
                n = 12
            try:
                return self.reply(200, {"notifications": notifications.history(min(max(n, 1), 50))})
            except Exception as e:
                return self.reply(502, {"error": str(e)[:120]})
        return self.reply(404, {"error": "not found"})

    def _do_notification(self, u):
        """POST /notifications (create), /notifications/ack, /notifications/clear.

        The create endpoint is how any Mac-side script raises a nudge for a specific agent:
        ``{"text": "...", "agent": "<profile>", "mood": "alert"}``. Same token gate as /turn.
        """
        if not self._authed():
            return self.reply(401, {"error": "unauthorized"})
        body = self._json_body()
        q = urllib.parse.parse_qs(u.query)

        def field(name):
            v = body.get(name)
            return v if v is not None else q.get(name, [None])[0]

        try:
            if u.path == "/notifications":
                # `due` (unix ts or seconds-from-now via due_in) makes it a reminder.
                due = field("due")
                if due is None and field("due_in") is not None:
                    due = int(time.time()) + int(field("due_in"))
                it = notifications.add(field("text"), agent=field("agent") or "",
                                       mood=field("mood") or "",
                                       due=int(due) if due else None)
                if not it:
                    return self.reply(400, {"error": "text required"})
                return self.reply(200, {"ok": True, "notification": it})
            if u.path == "/notifications/ack":
                return self.reply(200, {"ok": notifications.ack(field("id"))})
            if u.path == "/notifications/cancel":
                # DELETE, not ack: a cancelled reminder must not still exist to fire later.
                return self.reply(200, {"ok": notifications.cancel(field("id"))})
            if u.path == "/notifications/clear":
                kept = notifications.clear_history()
                return self.reply(200, {"ok": True, "kept": kept})
        except Exception as e:
            return self.reply(502, {"error": str(e)[:120]})
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path.startswith("/notifications"):
            return self._do_notification(u)
        if u.path != "/turn" or not self._authed():
            return self.reply(401, {"error": "unauthorized"})
        want_stream = "stream" in urllib.parse.parse_qs(u.query)
        try:
            t0 = time.time()
            q = urllib.parse.parse_qs(u.query)
            route = agents.resolve(q.get("profile", [""])[0])   # per-profile url/key/voice (spec 2)
            profile = route["name"]                             # "default" unless a real alt (or fallback)
            text = q.get("text", [""])[0]
            if not text:
                pcm = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                text = transcribe(pcm) if len(pcm) > 6400 else ""
            t_stt = time.time() - t0
            if not text:
                if want_stream:
                    return self.stream_no_speech()
                return self.reply(200, {"heard": "", "reply": ""})
            print("heard:", text, "profile:", profile, flush=True)
            if re.fullmatch(r"(?i)\W*(start|begin)?\s*(a )?(new|fresh) (conversation|chat|session)\W*|\W*start over\W*", text):
                clear_sid(route)
                return self.reply(200, {"heard": text, "reply": "Okay, starting a fresh conversation.", "react": "", "image_b64": ""})
            if want_stream:
                return self.do_POST_stream_stream(u, text, route)
            t1 = time.time()
            out, mood, image = ask_hermes(text, route)
            print("reply:", out[:80], "[%s]" % mood if mood else "", "[image]" if image else "",
                  "(stt %.1fs, hermes %.1fs)" % (t_stt, time.time() - t1), flush=True)
            self.reply(200, {"heard": text, "reply": out, "react": mood,
                             "image_b64": base64.b64encode(image).decode() if image else ""})
        except Exception as e:
            print("error:", e, file=sys.stderr, flush=True)
            self.reply(502, {"error": str(e)[:120]})

    def do_POST_stream_stream(self, u, text, route=None):
        """NDJSON: heard -> status -> tts chunks -> final. Real chunked framing (the gadget's
        HTTP client requires it; curl tolerates its absence, esp_http_client does not)."""
        route = route or {}
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        t1 = time.time()

        def push(obj):
            b = (json.dumps(obj) + "\n").encode()
            self.wfile.write(("%x\r\n" % len(b)).encode() + b + b"\r\n")
            self.wfile.flush()
            if os.environ.get("RELAY_DEBUG"):
                print("[stream +%5.1fs] %s" % (time.time() - t1, json.dumps(obj)[:80]), file=sys.stderr, flush=True)

        push({"heard": text})
        sid = load_sid(route)
        tts_queue = []           # (index, sentence) waiting to be synthesized
        tts_done = False
        text_acc = []            # the full reply, accumulated
        sent_cursor = 0          # how much of text_acc is already spoken/queued
        stream_mood = ""         # the [react:name] captured at the stream's head
        stream_image = ""        # an image path captured mid-stream (MEDIA:/[image:])
        tts_next_idx = 0         # index handed to the next queued sentence
        tts_send_idx = 0         # index the pusher has sent through (order-preserving)
        tts_out = {}             # idx -> encoded mp3, waiting for its turn
        tts_inflight = 0         # sentences popped by a worker, not yet in tts_out
        tts_lock = threading.Lock()

        SPOKEN_MAX = 240   # chars actually spoken per turn; the rest of the reply stays on screen
        spoken_chars = 0
        cap_logged = False

        def queue_sentence(sent):
            """Queue a sentence for speech while the spoken budget lasts (Chums-style brevity):
            past SPOKEN_MAX the reply keeps streaming as text but stops being spoken."""
            nonlocal spoken_chars, tts_next_idx, cap_logged
            with tts_lock:
                if spoken_chars + len(sent) > SPOKEN_MAX:
                    if not cap_logged:
                        cap_logged = True
                        print("[tts-cap] spoken budget (%d chars) reached; rest stays text" % SPOKEN_MAX,
                              file=sys.stderr, flush=True)
                    return
                tts_queue.append((tts_next_idx, sent))
                tts_next_idx += 1
                spoken_chars += len(sent)

        def tts_pump_out():
            """Send finished chunks in QUEUE order: synthesis overlaps, playback order holds.
             Exits only when the stream is done AND every queued or in-flight chunk has been
             sent — a worker holding the last sentence must not be cut off (a one-sentence
             reply would lose its whole audio otherwise)."""
            nonlocal tts_send_idx
            while True:
                with tts_lock:
                    if tts_send_idx in tts_out:
                        b64 = tts_out.pop(tts_send_idx)
                        tts_send_idx += 1
                    elif tts_done and not tts_queue and not tts_out and tts_inflight == 0:
                        return
                    else:
                        b64 = None
                if b64:
                    push({"tts": b64})
                else:
                    time.sleep(0.03)

        def synthesize_worker():
            nonlocal tts_inflight
            while True:
                with tts_lock:
                    item = tts_queue.pop(0) if tts_queue else None
                    if item is not None:
                        tts_inflight += 1
                if item is None:
                    if tts_done:
                        return
                    time.sleep(0.08)
                    continue
                idx, s = item
                # Last-line guard: no tag text, no path fragment, ever spoken. A sentence
                # that still carries one (a tag split across deltas outran the strip) is
                # dropped whole; path-shaped text only, so "saved as cat.png" is spoken.
                if is_tts_artifact(s):
                    print("[tts-drop] %r" % s[:100], file=sys.stderr, flush=True)
                    with tts_lock:
                        tts_out[idx] = None      # keep the order cursor moving past it
                        tts_inflight -= 1
                    continue
                mp3 = tts_synth(s, route.get("voice_name") or None)   # per-profile voice (#voice)
                with tts_lock:
                    tts_out[idx] = base64.b64encode(mp3).decode() if mp3 else None
                    tts_inflight -= 1

        workers = [threading.Thread(target=synthesize_worker, daemon=True) for _ in range(2)]
        for w in workers:
            w.start()
        pump = threading.Thread(target=tts_pump_out, daemon=True)
        pump.start()

        def on_status(s):
            if s:
                push({"status": s})
        raw_acc = []             # TRUE raw stream: all deltas, never stripped -- text_acc keeps the
                                 # STRIPPED copy for speech, so a tag split across deltas must not
                                 # lose its start marker (MEDIA:/paths were silently uncapturable)
        raw_full = ""            # last raw snapshot, so [remind:...] tags can be acted on at turn end
        def on_text(delta):
            nonlocal sent_cursor, stream_mood, stream_image, tts_next_idx, raw_full
            raw_acc.append(delta)
            raw = "".join(raw_acc)                   # untouched, for marker/limit detection
            raw_full = raw
            full = raw
            if not sent_cursor:   # the mood tag rides the stream's head: capture it there
                m2 = re.search(r"\[react[:=]\s*(\w+)", full)
                if m2:
                    stream_mood = m2.group(1).lower()
            # an image directive anywhere in the stream: capture its path
            # (run on raw so a tag split across deltas is caught once complete)
            mi2 = re.search(IMG_RX, raw)
            if mi2:
                stream_image = (mi2.group(1) or mi2.group(2)).strip()
            # strip ALL tag/marker text from the spoken/caption text, every tick
            full = re.sub(r"\[image:\s*[^\]]*\]", "", full)
            full = re.sub(r"\[react[^\]]*\]", "", full)
            full = re.sub(r"\s*MEDIA:[/\w.\- ]*", "", full)
            text_acc[:] = [full]
            # HOLD-BACK on the RAW text: a marker near the tail (a tag still streaming in —
            # an unterminated '[' or a bare path fragment) means the boundary is not safe yet.
            nonlocal_limit = len(full)
            for marker in ("[", "MEDIA:", "/Users/", "http://", "https://"):
                p = raw.rfind(marker)
                if p > sent_cursor:
                    tail = raw[p:]
                    closed = ("]" in tail) if marker == "[" else (
                        re.search(r"\.(?:png|jpe?g|gif|webp)\b", tail) is not None or marker == "http://x")
                    if not closed:
                        nonlocal_limit = min(nonlocal_limit, max(p, sent_cursor))
            # queue every complete sentence beyond the cursor, before the hold-back line
            while True:
                m = _SENT_RE.search(full, sent_cursor)
                if not m or (m.end() > nonlocal_limit and nonlocal_limit < len(full)):
                    break
                sent = m.group(0).strip()
                if sent:
                    if os.environ.get("RELAY_DEBUG"):
                        print("[tts-queue] %r" % sent[:120], file=sys.stderr, flush=True)
                    queue_sentence(sent)
                sent_cursor = m.end()
            # queue every complete sentence beyond the cursor for TTS
            # (a tag split across deltas leaves an unterminated '[' or a bare 'MEDIA:'/'http'
            # prefix in the tail: hold it back — only boundaries before such a marker are safe,
            # else the half-tag is spoken)
            limit = len(full)
            for marker in ("[", "MEDIA:", "http://", "https://"):
                p = full.rfind(marker)
                if p > sent_cursor and p < limit:
                    # hold only if the construct is unterminated at the tail
                    tail = full[p:]
                    closed = "]" in tail if marker == "[" else (
                        re.search(r"\.(png|jpe?g|gif|webp)\b", tail) is not None)
                    if not closed:
                        limit = min(limit, p)
            while True:
                m = _SENT_RE.search(full, sent_cursor)
                if not m or m.end() > limit:
                    break
                sent = m.group(0).strip()
                if sent:
                    if os.environ.get("RELAY_DEBUG"):
                        print("[tts-queue] %r" % sent[:120], file=sys.stderr, flush=True)
                    queue_sentence(sent)
                sent_cursor = m.end()

        try:
            for attempt in range(3):
                try:
                    rj, new_sid = hermes_call_stream_full(text, sid, on_status, on_text, route)
                    break
                except urllib.error.HTTPError as e:
                    if e.code != 429 or attempt == 2:
                        raise
                    print("gateway busy (429), retrying in %ds" % (12 * (attempt + 1)), file=sys.stderr, flush=True)
                    time.sleep(12 * (attempt + 1))
            if new_sid and new_sid != sid:
                save_sid(route, new_sid)
            used = (rj.get("usage") or {}).get("prompt_tokens", 0)
            if SESSION_RESET_TOKENS and used > SESSION_RESET_TOKENS:
                print("session prompt at %d tokens: resetting" % used, file=sys.stderr, flush=True)
                clear_sid(route)
        except Exception as e:
            print("hermes failed:", e, file=sys.stderr, flush=True)
            s2 = str(e).lower()
            msg = ("Still working on that one, ask me again in a moment." if "timed out" in s2
                   else "I can't reach the Hermes gateway right now." if ("refused" in s2 or "unreachable" in s2)
                   else "Sorry, something went wrong there.")
            raw_msg = ""   # failure path: no image capture, just the fail message
        else:
            msg = "".join(text_acc).strip()
            raw_msg = "".join(raw_acc)   # raw (unstripped) for image capture: text_acc had tags eaten mid-stream
            msg = re.sub(r"\[image:\s*[^\]]*\]", "", msg).strip()   # the tag never reaches speech/captions
            msg = _REMIND_TAG.sub("", msg).strip()   # and the reminder tag
            # act on any [remind:...] the reply carried. The RAW accumulated text is
            # the reliable source (a tag split across deltas stays whole there).
            extract_reminders(raw_msg or raw_full or msg, agent=route["name"])
        tts_done = True
        for w in workers:
            w.join(timeout=30)
        pump.join(timeout=15)
        msg = re.sub(r"\[react[^\]]*\]\s*", "", msg)   # any leaked meta-react bracket is never spoken
        out, mood = _reply_lines(msg)
        if stream_mood and not mood:
            mood = stream_mood if stream_mood in MOODS else ""
        image = b""
        # capture the image from the RAW stream text: text_acc was stripped every tick, so a
        # MEDIA:/path whose start marker was eaten mid-delta survives only here.
        stream_image_final = stream_image or (IMG_RX.search(raw_msg).group(1) or IMG_RX.search(raw_msg).group(2)
                                              if IMG_RX.search(raw_msg) else "")
        if stream_image_final:
            try:
                image = prepare_image(stream_image_final)
            except Exception as e:
                print("image failed:", e, file=sys.stderr, flush=True)
                out += " I couldn't show that image."
        push({"reply": out, "react": mood,
              "image_b64": base64.b64encode(image).decode() if image else "",
              "done": True})
        self.wfile.write(b"0\r\n\r\n")   # the chunked body's terminator
        self.wfile.flush()
        print("reply:", out[:80], "[%s]" % mood if mood else "", "[image]" if image else "",
              "(stt %.1fs, hermes %.1fs, streamed)" % (t_stt, time.time() - t1), flush=True)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    def _warm():
        # Held while starting so transcribe_worker waits instead of racing; released even on
        # failure, or the first turn would block on this lock forever (the old lambda leaked it).
        _worker_lock.acquire()
        try:
            _start_worker()
        except Exception as e:
            print("whisper warmup failed (%s); the first turn will retry" % e, file=sys.stderr, flush=True)
        finally:
            _worker_lock.release()

    migrate_legacy_session()

    threading.Thread(target=_warm, daemon=True).start()   # warm the model
    print(f"relay on :{PORT} -> {HERMES}; Google STT key {'found' if read_google_key() else 'MISSING'}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
