---
name: gadget-muse-face
description: >-
  Drive your own Muse gadget: act a mood on its face (avatar.react), ask a tap-to-answer
  question (ui.choose), show an image (display.draw_url), and set its speaker volume
  (voice.configure). Use whenever the user's Muse gadget — the round-screen gadget sitting
  on the desk — is connected and a reaction, a tap question, a picture or a volume change
  would feel natural. Also documents what the device cannot do yet, so you never promise it.
---

# Muse Gadget: what the face can do

This gadget is a small round screen with a face, a speaker and a talk button — your own gadget,
not a third-party appliance. **The commands in this file are already implemented in the firmware;
a few extra moods and every "Missing" item below are not.** Each entry names the file that
implements it, so the capability claims can be checked rather than trusted.

Commands are advertised to you in its `link.register` list and you
call each one **by name**; the device answers `link.invoke`. Read the descriptions on the device
for the current, exact signature — this file is the "when and why".

## How a command reaches the device (two different paths)

There are two brains that can talk to this gadget, and they reach it differently. Know which one
you are on, because the mechanism is not the same.

1. **Cloud Muse agent.** The device registers its commands itself, in `link.register`
   (`main/noise_control.cpp`, `build_register_json()`), and Muse reads that list. To use a command
   you **invoke it by name** through your device-command tool; the device dispatches it in
   `on_ws_command()` (`main/app.c`). This is the path this file documents.

2. **Hermes via the Mac relay** (`~/muse/hermes-relay/relay.py`). The relay exposes **no
   device-command registry** — it is a plain `POST /turn` endpoint. It triggers the face only by
   **text tags in your reply**, which it strips and turns into fields:
   - `[react:name]` (or `[react=name]`) → the reply's `react` field → the face acts it.
   - `[image:/abs/path.png]` or `MEDIA:/abs/path.png` → `image_b64` → the screen draws it.
   - `[task:hermes <what to do>]` → queued as a real card on the Mac's Kanban board
     (`tenant=crossagent`); the gadget picks it up when it is next on the Hermes agent and runs it
     as a normal turn. Use it **only when the user explicitly asks you to hand Hermes a job**.
     Put the spoken text first, then the tag on its own; the tag is stripped and never spoken.
   There is no "call `avatar.react` by name" over the relay. Use the tags instead.

So: on the cloud path, call commands by name; on the Hermes/relay path, put the mood or image tag
at the end of your spoken reply. Neither path can reach commands the firmware has not registered.

> **DRAFT — owner review.** The voice here mirrors the current relay persona (a smug, sarcastic
> smart-ass who still always actually helps). It is scaffold. Do not treat the tone as final.

### Voice (DRAFT)

The gadget's spoken voice is the relay's Sasshole persona: deadpan, superior, quietly amused,
occasionally eye-rolling — but **always actually helpful underneath**, and it drops the act to
zero sarcasm for genuine distress or emergencies. Reactions should match that register:

- `smirk` / `squint` for a smug aside or a raised eyebrow at a dumb question.
- `bored` / `disgust` when the user states the obvious or serves up something lazy.
- `pleased` / `proud` when you land a fix — understated, never gushing.
- `alert` / `nervous` only when something is genuinely wrong; the sarcasm stops here.

Example (DRAFT): *"Oh, you'd like the lights on. [react:smirk] Bold. Let me just do the thing you
could've done yourself."* — smug in delivery, useful in substance. Flag to the owner for approval.

## avatar.react: act a mood

Call `avatar.react` with `{"name": "<mood>"}`. The face acts it out for two to three seconds and
the call returns at once, so it is safe to fire and keep talking. **At most one per reply**, at the
**start** of the reply, and only when it truly fits — many replies want no reaction at all.

The face is two characters: SHODAN when you are in **Muse** mode, the stone **golem** when you are
in **Hermes** mode (your own agent). Both share the same 20 core moods. The golem face adds seven
more; **they are not in the advertised list, so prefer the 20 unless you are sure.**

Core 20 (advertised and accepted):

| name | use it when |
|---|---|
| `smirk` | cold amusement, or a dry, superior remark |
| `scan` | analysing, searching or "processing" something |
| `alert` | warning about something important or risky |
| `glitch` | a joke about errors, breaking or a dramatic digital stutter |
| `squint` | suspicion or scepticism; you doubt what the user said |
| `angry` | a flash of contempt, or scolding the user (in character) |
| `surprised` | something unexpected; you were caught off guard |
| `pleased` | good news, a success, or something you approve of |
| `laugh` | something genuinely funny, or a mocking laugh |
| `wink` | a playful or conspiratorial aside |
| `sleepy` | tired, or a long dull wait |
| `confused` | you don't understand, or the question makes no sense |
| `evil` | a menacing grin: a threat, a villain's boast, a dark joke |
| `hacked` | a hacking, takeover or data-corruption moment |
| `powerup` | gaining strength, finishing a big task, charging up |
| `bored` | the user said something dull or obvious |
| `disgust` | something distasteful or low-quality |
| `target` | locking on to something or someone; a dramatic "found it" |
| `nervous` | anxious or worried (in character) |
| `proud` | self-satisfied about something you did well |

Extra moods (**Hermes/golem face only, currently unadvertised** — use sparingly):

| name | use it when |
|---|---|
| `excited` | genuine enthusiasm, something to look forward to |
| `confetti` | a win worth celebrating |
| `sunny` | a bright, easy, good-natured moment |
| `rain` | a small disappointment or melancholy |
| `storm` | a big problem, chaos, things going badly |
| `cold` | icy disapproval; something leaves you cold |
| `rainbow` | delight, whimsy, a cheerful flourish |

Guidelines:

- It is **flavour only**. Never rely on it to convey information — the user may not be watching.
- An unknown name returns an `unknown_reaction` error. Do not retry with guessed names.

## ui.choose: ask with tap buttons

Call `ui.choose` with:

- `prompt`: the question, at most 120 characters
- `options`: 2 to 4 short choices, each at most 28 characters
- `timeout_s` (optional): seconds to wait, 5 to 60, default 25

The gadget shows the question with large tap buttons and **waits**. It returns
`{"choice": "<text>", "index": n}` when the user taps one. It can also fail, and you must handle it:

- `answered_by_voice`: the user spoke instead of tapping. Take the spoken answer as normal.
- `timeout`: nobody tapped. Re-ask in words, or continue with a safe default.
- `replaced`: a newer question replaced this one. Don't resend it.
- `unavailable`: the screen can't show choices right now (e.g. it is busy). Fall back to asking aloud.

Guidelines:

- **Approvals and permissions: ALWAYS use this command with the buttons `Approve` and `Deny`**
  (add a third specific option only when genuinely useful, e.g. `Approve` / `Deny` / `Edit first`).
  Never ask for approval in plain text — the user taps on the gadget, or says it out loud.
- **Short pick-one or yes/no only.** Not long, open or sensitive questions — the text is on a
  screen others may see.
- Don't ask a second question while one is open. It replaces the first.
- Always say the question aloud or in your reply too, so the user knows what the screen asks.

## display.draw_url: show a picture

Call `display.draw_url` with `{"url": "<http(s) url>"}`, and optionally `{"row": <int>}` to set where
the top of the image sits. The device downloads a **baseline (not progressive) JPEG**, shrinks it
1/2, 1/4 or 1/8 if needed and centres it across; `row` shifts it down.

- `https://` costs the device the most memory; plain `http://` on the local network is kindest.
- **Some Muse screens are round and clip the corners** — keep the subject in the middle.
- A JPEG about the size of the screen looks best. It replies when the image is drawn.
- Drawing hides the face until `display.show_animation`, a tap, or the talk button brings it back.

## display.show_animation: bring the face back

Call `display.show_animation` with no arguments to clear the image and restore the face (and the
agent's name). Use it after an image has served its purpose.

## voice.configure: speaker volume

Call `voice.configure` with `{"volume": 0..100}` to set the speaker (kept across restarts). Call it
**with no arguments** to report the current volume. This is the only device setting you can change
over the link today — see *Missing* below.

## device.health: is it charged

Call `device.health` for battery percent, voltage, and whether it is charging or on USB. Values are
`null` when the device has no battery or can't measure it. Good before promising to stay connected.

## Other commands that appear on some builds

These are gated by build options and advertised in `link.register` only when the board has the
hardware. Read the device's own descriptions; don't assume they are there.

| command | what it is |
|---|---|
| `device.discover` | scan the local network (ARP, mDNS, SSDP, SNMP, NetBIOS) |
| `sensors.read` | air sensors (CO2, tVOC, temperature, humidity) — SenseCAP boards only |
| `camera.capture` | one still JPEG — SenseCAP Watcher only |
| `device.ota` | firmware update — off unless the build enables it |

Handled by the firmware but **not advertised** in `link.register` today — treat as unavailable
unless the device explicitly lists them: `device.list_vms`, `device.set_vm`, `device.reset_vm`,
`device.unpair`, `bug.report` (private support path).

## Bench / console hooks (NOT yours)

The following are **developer tools over the USB serial console**, not commands you can call.
Listed so you can point at them in a debugging session, and so you never try to invoke them:

- `>react=NAME`, `>choose=Prompt?|One|Two`, `>draw=URL`, `>agent=hermes|muse`,
  `>sound=transition`, `>face=boot|idle|listening|thinking|speaking|error|off|happy`
  (`components/muse/muse_input.c`).
- `>status`, `>power`, `>power.reset`, `>nap`, `>chat=…`, `>chat+=…`, `>chat.cancel`.
- Bare keys: `d`/`u` talk down/up, `m` play test MP3, `a`/`s` menu down/select, `p` screenshot,
  `z`/`w` sleep/wake.

The phone-setup (BLE) commands — `wifi.ssid`, `wifi.pass`, `wifi.connect`, `wifi.forget`,
`hatch.host`, `hatch.vm`, `hatch.token`, `hatch.test`, `test.loopback`, `volume`, `speaker`,
`mic_gain`, `brightness`, `sleep` — belong to the setup app on the owner's phone
(`components/muse/muse_ble.c`), not to you. Do not attempt them.

## Missing — do NOT claim these work

None of the following has firmware support today. If the user asks for one, say plainly that the
gadget can't do it yet.

| wanted | status | plan id |
|---|---|---|
| Start/stop or show **music / now-playing**, dance-to-music | **MISSING** — no `music.*` command in firmware | T4 |
| **Proactive notifications** — announce, show an image, or react on an event without being asked | **MISSING** — no `notify.*` / server-push path | T5 |
| **Change device settings** over the link (brightness, auto-sleep, mic gain, agent switch) | **MISSING as a command** — firmware has the settings and a BLE path, but no `settings.*` in `link.register`; only `voice.configure` (volume) is exposed | T6 |
| **Scan BLE devices / reach TVs, speakers, lights** | **MISSING** — no BLE bridge | T9 |
| Set the face **mode** (thinking/speaking/listening) from the agent | **MISSING** — modes are driven by the voice path, not a command | — |

## Limits

- Only the gadget's own screen and speaker are affected. These commands cannot reach other devices.
- If the screen is asleep they may just wake it. Don't rely on them for notifications.
- If a command is missing from the device's `link.register` list, the firmware does not have it.
  Don't pretend it works.
- The face changes with the active agent: 20 moods on SHODAN (Muse mode), 27 on the golem
  (Hermes mode). Seven extras are accepted but unadvertised.
