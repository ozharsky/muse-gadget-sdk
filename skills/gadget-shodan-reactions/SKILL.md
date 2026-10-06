---
name: gadget-shodan-reactions
description: >-
  Act out short moods on the user's Muse gadget's face (avatar.react) and ask quick pick-one
  questions on its touch screen (ui.choose). Use when the user's Muse gadget (a round-screen
  gadget with a SHODAN-style avatar) is connected and a reaction or a tap-to-answer question
  would feel natural.
---

# Muse Gadget: Face Reactions and Tap Questions

This gadget has a face on a round screen. Two commands, listed in the gadget's registration, let you
use it. Call them by name through your device command tool. Neither needs any setup.

## avatar.react: act a mood

Call `avatar.react` with `{"name": "<mood>"}`. The face acts the mood out for about two to three seconds and the
call returns at once. The moods:

| name | use it when |
|---|---|
| `smirk` | cold amusement, or a dry, superior remark |
| `scan` | analysing, searching or "processing" something |
| `alert` | warning the user about something important or risky |
| `glitch` | a joke about errors, breaking or a dramatic digital stutter |
| `squint` | suspicion or scepticism; you doubt what the user said |
| `angry` | a flash of contempt, or scolding the user (in character) |
| `surprised` | something unexpected; you were caught off guard |
| `pleased` | good news, a success, or the user did something you approve of |
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

Guidelines:

- Use it **at most once per reply**, and only when it truly fits the moment. Many replies need no reaction at all.
- Send it **at the start of your reply**, so the face acts while the gadget speaks.
- It is flavour only. Never rely on it to convey information: the user may not be looking at the screen.
- An unknown name returns `unknown_reaction`. Don't retry with guessed names.

## ui.choose: ask with tap buttons

Call `ui.choose` with:

- `prompt`: the question, at most 120 characters
- `options`: 2 to 4 short choices, each at most 28 characters
- `timeout_s` (optional): seconds to wait, 5 to 60, default 25

The gadget shows the question with large tap buttons and **waits**. It returns `{"choice": "<text>", "index": n}`
when the user taps one.

It can also fail, and you must handle that gracefully:

- `answered_by_voice`: the user spoke instead of tapping. Take their spoken answer from the conversation as normal.
- `timeout`: nobody tapped. Ask again in words, or carry on with a sensible default if it is safe to.
- `replaced`: a newer question replaced this one. Don't resend it.

Guidelines:

- **Approvals and permissions: ALWAYS use this command with the buttons `Approve` and `Deny`** (add a third, specific option only when it is genuinely useful, e.g. `Approve` / `Deny` / `Edit first`). Never ask for approval in plain text — the user taps the button on the gadget, or says "approve"/"deny" out loud.
- Use it only for **short pick-one or yes/no questions** where tapping is easier than talking: choosing between a few options, confirming an action, picking a time.
- Do not use it for long or open questions, or for anything sensitive. The text appears on a screen others may see.
- Keep each option a few words. Labels longer than about 28 characters are rejected.
- Don't ask a second question while one is open. It replaces the first.
- Always say the question aloud or in your reply too, so the user knows what the screen is asking.

## Limits

- Only the gadget's own screen is affected. These commands cannot reach other devices.
- If the screen is asleep they may only wake it, so don't rely on them for notifications.
- If the commands are missing from the gadget's list, the firmware does not have them. Don't pretend they work.
