"""Keeps a Whisper model loaded: reads a WAV path per line on stdin, prints the transcript (one line) on stdout.

Prefers faster-whisper (CTranslate2): the same base.en weights, ~2.5x faster on this Mac than
the openai-whisper torch build (0.5-1.6 s vs 1.4-2.4 s warm). Falls back to openai-whisper
if faster_whisper is not importable (the old Homebrew interpreter). Recreate the fast venv:

    cd ~/muse/hermes-relay && uv venv venv-stt && uv pip install --python venv-stt/bin/python faster-whisper

The relay always writes 16 kHz mono PCM16 WAVs, so the worker decodes the bytes itself
(wave + numpy) and hands the model a float32 array -- decode stays in-process and does not
depend on the fast-whisper `av` path-decoding compatibility.

Knobs: WHISPER_MODEL (default base.en), WHISPER_BEAM (default 5), WHISPER_VAD (default 1).
"""
import os
import sys

MODEL = os.environ.get("WHISPER_MODEL", "base.en")
BEAM = int(os.environ.get("WHISPER_BEAM", "5"))
VAD = os.environ.get("WHISPER_VAD", "1") == "1"


def load_wav(path):
    """16 kHz mono PCM16 -> float32 in [-1, 1]; None if it is not that exact format."""
    import wave
    import numpy as np
    with wave.open(path, "rb") as w:
        if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
            return None
        frames = w.readframes(w.getnframes())
    return np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0


try:
    from faster_whisper import WhisperModel

    model = WhisperModel(MODEL, device="cpu", compute_type="int8")
    ENGINE = "faster-whisper"

    def transcribe(path):
        audio = load_wav(path)
        segments, _ = model.transcribe(audio if audio is not None else path, language="en",
                                       beam_size=BEAM, vad_filter=VAD,
                                       condition_on_previous_text=False)
        return " ".join(s.text.strip() for s in segments).strip()
except ImportError:   # the Homebrew openai-whisper interpreter
    import whisper

    model = whisper.load_model(MODEL)
    ENGINE = "openai-whisper"

    def transcribe(path):
        audio = load_wav(path)
        return model.transcribe(audio if audio is not None else path, language="en", fp16=False,
                                condition_on_previous_text=False)["text"].strip()

print("whisper worker: %s, model %s, beam %d, vad %s" % (ENGINE, MODEL, BEAM, VAD),
      file=sys.stderr, flush=True)
print("ready", flush=True)

for line in sys.stdin:
    path = line.strip()
    try:
        text = transcribe(path)
    except Exception as e:   # never die: answer empty and stay loaded
        text = ""
        print("worker error:", e, file=sys.stderr, flush=True)
    print(text.replace("\n", " "), flush=True)
