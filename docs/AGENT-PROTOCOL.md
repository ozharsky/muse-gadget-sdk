# The agent wire contract — how the gadget talks to a server

The gadget is an agent *client*. It records your voice (or takes console text), POSTs it
to an HTTP base URL it is configured with, and speaks the reply that comes back. Any
server implementing the small contract below works. The reference implementation
(the `relay/` folder, stdlib Python with pluggable STT/TTS) ships in this repo;
nothing in the firmware requires it.

## Configuration (firmware side)

All three live in your board profile (`idf.py menuconfig` → *ESP32 Device SDK*):

- `CONFIG_MUSE_HERMES_URL` — base URL used at home / on your LAN (plain `http` is fine).
- `CONFIG_MUSE_HERMES_URL_REMOTE` — second base URL used when the home one is unreachable
  (e.g. a Tailscale funnel or any public `https` endpoint). Optional.
- `CONFIG_MUSE_HERMES_TOKEN` — shared secret, sent as `X-Gadget-Token` on every request.

The base URL must **not** end in `/turn` — the firmware appends the path. With the URLs
left empty the gadget simply has no relay agent; the Muse cloud path still works.

## The endpoints

### `POST {base}/turn` — one voice note or typed line

- **Voice:** body = signed 16-bit little-endian mono PCM at 16 kHz (written with a `RIFF`
  WAV header). Query adds `&profile=<name>` when an ALT agent is active.
- **Typed** (console bench): `POST {base}/turn?text=<urlencoded>`, empty body.
- **Header:** `X-Gadget-Token: <your secret>` on everything; reject the rest with 401.

The firmware tries the home URL first and falls back to the remote one when the home
address is unreachable from the device's current network.

**Non-streaming answer** — `200` + JSON:

```json
{"heard": "what STT made of the note (shown while waiting)",
 "reply": "the text to speak",
 "react": "optional mood name",
 "image_b64": "optional base64 JPEG to show"}
```

**Streaming answer** — add `?stream=1`; the response is chunked NDJSON, one JSON object
per line. The device starts playing audio as it arrives and paints captions live:

```
{"heard": "..."}                        # optional, first
{"status": "searching the web..."}      # optional tool-progress captions
{"tts": "<base64 MP3, one sentence>"}   # optional, repeatable — spoken as it arrives
{"reply": "...", "react": "...", "image_b64": "...", "done": true}   # final line
```

Chunked framing (hex length + CRLF per chunk, terminating `0\r\n\r\n`) is mandatory — the
device's HTTP client rejects unframed bodies that curl tolerates. If you never send `tts`
lines the device synthesizes the whole reply itself (when a TTS key is baked in) or paces
the captions silently (when it isn't). The final `done` line is mandatory in both cases.

### Notifications — the optional reverse channel

If your server also wants to push unprompted lines (print finished, a reminder came due,
a build failed):

- `GET {base}/notifications` — the device polls this (15 s awake / 60 s asleep, only
  while a Hermes-mode agent is active).
- `POST {base}/notifications` — `{"text","agent","mood","due_in"|"due"}` raises one.
- `POST {base}/notifications/ack` — `{"id"}` marks it presented.

Skip all of it and the device just never hears a nudge. Same token on every call.

## Timing the firmware expects

| Stage | Budget |
|---|---|
| HTTP timeout (whole call) | 100 s |
| First reply text, voice (agent idle) | ~60 s, then it keeps waiting to the hard cap |
| Voice-turn hard cap | 180 s (typed turns: 15 min) |

A voice turn that reaches the cap with **zero** reply text reports "NO REPLY" on screen —
always send *something*: a first sentence, or a status line that buys you time.

## Rules of thumb for a good voice agent

- Keep replies short — they get **spoken**. One to three sentences, no markdown; the
  device strips common tags but nothing replaces actual brevity.
- Emit the first `tts` sentence as soon as you have one — that is what makes it feel fast.
- `react` names come from the firmware's mood set (`components/muse/muse_idle.h` and the
  baked mood tables); an unknown name is dropped harmlessly.
- `image_b64`: a ≤ ~100 KB JPEG; square crops read best on the round panel.

## Minimal server

```python
@app.post("/turn")                      # any framework; this is the whole shape
def turn(request):
    if request.headers["X-Gadget-Token"] != SECRET:
        return 401
    text = stt(request.body) if request.body else request.args["text"]
    reply = my_agent(text)             # your LLM, your script, your rules
    return {"heard": text, "reply": reply}
```

Streaming and the notification store are the two optional upgrades — the reference
relay (`hermes-relay`) does both if you'd rather start from a working one.
