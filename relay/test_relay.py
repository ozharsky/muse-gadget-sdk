"""Regression tests for the relay's reply post-processing (no server, no network)."""
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agents
import relay


class MoodTests(unittest.TestCase):
    def test_leading_tag(self):
        self.assertEqual(relay.extract_mood("[react:scan] Hello there."), ("scan", "Hello there."))

    def test_equals_form_anywhere(self):
        self.assertEqual(relay.extract_mood("Hello [react=scan] there."), ("scan", "Hello there."))

    def test_whitespace_in_tag(self):
        self.assertEqual(relay.extract_mood("[react: scan]\nHi."), ("scan", "Hi."))

    def test_unknown_mood_stripped_but_ignored(self):
        self.assertEqual(relay.extract_mood("[react:bogus] Hi."), ("", "Hi."))

    def test_trailing_tag(self):
        self.assertEqual(relay.extract_mood("Hello [react:scan]"), ("scan", "Hello"))

    def test_no_tag(self):
        self.assertEqual(relay.extract_mood("No tag at all."), ("", "No tag at all."))

    def test_two_tags_all_stripped(self):
        mood, out = relay.extract_mood("[react:scan] One. [react:sunny] Two.")
        self.assertEqual(mood, "scan")
        self.assertNotIn("[react", out)


class CapTests(unittest.TestCase):
    def test_short_passthrough(self):
        self.assertEqual(relay.cap_reply("Short.", limit=100), "Short.")

    def test_cuts_at_sentence_end(self):
        reply = ("A sentence about something. " * 40) + "tail"
        out = relay.cap_reply(reply, limit=200)
        self.assertLessEqual(len(out), 200)
        self.assertTrue(out.endswith("."))

    def test_hard_cut_without_sentence(self):
        out = relay.cap_reply("x" * 500, limit=200)
        self.assertEqual(len(out), 200)

    def test_default_limit_matches_module(self):
        out = relay.cap_reply("y" * (relay.MAX_REPLY + 50))
        self.assertLessEqual(len(out), relay.MAX_REPLY)


class NoticeTests(unittest.TestCase):
    def test_notice_in_middle(self):
        out = relay.strip_notice("Heads up: memory is off for this chat. Here's your joke: hi.")
        self.assertEqual(out.strip(), "Here's your joke: hi.")

    def test_notice_at_end(self):
        out = relay.strip_notice("Yep, I'm online and ready. Quick heads-up: Honcho memory is off for this chat.")
        self.assertEqual(out.strip(), "Yep, I'm online and ready.")

    def test_normal_reply_untouched(self):
        s = "I remember things fine. No metadata here."
        self.assertEqual(relay.strip_notice(s), s)


class TtsArtifactTests(unittest.TestCase):
    """The streaming TTS filter drops path/tag-shaped fragments but must speak normal
    sentences that merely mention a filename."""

    def test_bare_filename_is_spoken(self):
        self.assertFalse(relay.is_tts_artifact("The screenshot saved as cat.png earlier."))

    def test_path_fragment_dropped(self):
        self.assertTrue(relay.is_tts_artifact("sure, it went to /Users/you/Downloads/x.png"))

    def test_media_tag_dropped(self):
        self.assertTrue(relay.is_tts_artifact("done MEDIA:/tmp/photo.jpg"))

    def test_url_dropped(self):
        self.assertTrue(relay.is_tts_artifact("see https://example.com/thing for details"))

    def test_image_fragment_dropped(self):
        self.assertTrue(relay.is_tts_artifact("here [image:/tmp/a.png"))

    def test_react_fragment_dropped(self):
        self.assertTrue(relay.is_tts_artifact("nice [react:scan"))


class NoSpeechStreamTests(unittest.TestCase):
    """POST /turn?stream=1 with an empty transcript must answer NDJSON (heard "" + a
    done line), not a bare JSON body the gadget reads as 'relay unreachable'."""

    @classmethod
    def setUpClass(cls):
        import threading as _t
        cls.srv = relay.ThreadingHTTPServer(("127.0.0.1", 0), relay.H)
        cls.port = cls.srv.server_address[1]
        cls.t = _t.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.t.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def _post_empty(self, query):
        import urllib.request
        req = urllib.request.Request(
            "http://127.0.0.1:%d/turn%s" % (self.port, query), method="POST", data=b"",
            headers={"X-Gadget-Token": relay.TOKEN})
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.read().decode()

    def test_empty_stream_request_answers_ndjson(self):
        body = self._post_empty("?stream=1")
        lines = [json.loads(l) for l in body.splitlines() if l.strip()]
        self.assertGreaterEqual(len(lines), 2)
        self.assertEqual(lines[0].get("heard"), "")
        self.assertTrue(lines[-1].get("reply"))
        self.assertTrue(lines[-1].get("done"))

    def test_empty_plain_request_keeps_json_shape(self):
        body = self._post_empty("")
        obj = json.loads(body)
        self.assertEqual(obj.get("heard"), "")


class KeyFileTests(unittest.TestCase):
    """read_api_key_file must find the key regardless of the api_server sub-key order (the old
    rigid regex missed marketing's extra-before-enabled block) and never throw on a bad file."""

    GOOD = ("platforms:\n  api_server:\n    enabled: true\n    extra:\n"
            "      host: 127.0.0.1\n      key: goodkey123_abc\n      port: 8642\n")
    EXTRA_FIRST = ("platforms:\n  api_server:\n    extra:\n"
                   "      host: 127.0.0.1\n      port: 8644\n      key: fCFakeKey_xyz789\n    enabled: true\n")
    NO_KEY = "platforms:\n  api_server:\n    enabled: true\n    extra:\n      host: 127.0.0.1\n      port: 8642\n"
    QUOTED = "api_server:\n  enabled: true\n  extra:\n    key: \"qkey-42\"\n"

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def _write(self, content):
        p = os.path.join(self.dir, "config.yaml")
        with open(p, "w") as f:
            f.write(content)
        return p

    def test_reads_key_enabled_before_extra(self):
        self.assertEqual(agents.read_api_key_file(self._write(self.GOOD)), "goodkey123_abc")

    def test_reads_key_extra_before_enabled(self):
        self.assertEqual(agents.read_api_key_file(self._write(self.EXTRA_FIRST)), "fCFakeKey_xyz789")

    def test_block_without_key_is_none(self):
        self.assertIsNone(agents.read_api_key_file(self._write(self.NO_KEY)))

    def test_missing_file_is_none(self):
        self.assertIsNone(agents.read_api_key_file(os.path.join(self.dir, "nope.yaml")))

    def test_strips_surrounding_quotes(self):
        self.assertEqual(agents.read_api_key_file(self._write(self.QUOTED)), "qkey-42")

    def test_real_default_config_has_a_key(self):
        self.assertTrue(agents.read_api_key_file("~/.hermes/config.yaml"))

    def test_real_profile_config_key_is_read(self):
        # regression: the live gateway authenticates the default profile's key, so the loader must read it
        self.assertTrue(agents.read_api_key_file("~/.hermes/config.yaml"))


class ProfileRoutingTests(unittest.TestCase):
    """resolve() maps a profile name to url/key/voice: default unchanged, /p/<name>/ for an alt,
    and ALWAYS default (with a warning) for unknown / invalid / unusable profiles."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

        def cfg(name, key):
            p = os.path.join(self.dir, name)
            with open(p, "w") as f:
                f.write("api_server:\n  enabled: true\n  extra:\n    key: %s\n" % key)
            return p

        self.good = cfg("default.yaml", "DEFAULTKEY")
        self.trad = cfg("trading.yaml", "TRADINGKEY")
        self.nokey = cfg("nokey.yaml", "")
        self.agents = os.path.join(self.dir, "agents.json")
        with open(self.agents, "w") as f:
            json.dump({
                "default": {"label": "Hermes", "voice": "sasshole", "api_key_file": self.good},
                "trading": {"label": "Hermes Trading", "voice": "professional", "api_key_file": self.trad},
                "broken": {"label": "Broken", "voice": "professional", "api_key_file": self.nokey},
                "ghost": {"label": "Ghost", "voice": "plain", "api_key_file": os.path.join(self.dir, "gone.yaml")},
            }, f)
        self.base = "http://127.0.0.1:8642"

    def _resolve(self, name):
        return agents.resolve(name, base=self.base, path=self.agents)

    def test_default_route_is_the_plain_path(self):
        r = self._resolve("")
        self.assertEqual(r["name"], "default")
        self.assertEqual(r["api_url"], self.base + "/v1/chat/completions")
        self.assertEqual(r["api_key"], "DEFAULTKEY")
        self.assertEqual(r["voice"], "sasshole")
        self.assertFalse(r["fallback"])

    def test_explicit_default_word_is_the_plain_path(self):
        r = self._resolve("default")
        self.assertEqual(r["name"], "default")
        self.assertEqual(r["api_url"], self.base + "/v1/chat/completions")

    def test_none_and_whitespace_are_default(self):
        self.assertEqual(self._resolve(None)["name"], "default")
        self.assertEqual(self._resolve("   ")["name"], "default")

    def test_named_profile_selects_p_path_and_its_key_and_voice(self):
        r = self._resolve("trading")
        self.assertEqual(r["name"], "trading")
        self.assertEqual(r["api_url"], self.base + "/p/trading/v1/chat/completions")
        self.assertEqual(r["api_key"], "TRADINGKEY")
        self.assertEqual(r["voice"], "professional")
        self.assertFalse(r["fallback"])

    def test_unknown_profile_falls_back_to_default_with_warning(self):
        import contextlib, io
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            r = self._resolve("does_not_exist")
        self.assertEqual(r["name"], "default")
        self.assertTrue(r["fallback"])
        self.assertEqual(r["api_url"], self.base + "/v1/chat/completions")
        self.assertIn("does_not_exist", err.getvalue())
        self.assertIn("routing to default", err.getvalue())

    def test_unreadable_key_profile_falls_back_to_default_with_warning(self):
        import contextlib, io
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            r = self._resolve("broken")           # key file present but no key in it
        self.assertEqual(r["name"], "default")
        self.assertTrue(r["fallback"])
        self.assertIn("broken", err.getvalue())

    def test_missing_key_file_profile_falls_back(self):
        r = self._resolve("ghost")               # api_key_file points at a nonexistent config
        self.assertEqual(r["name"], "default")
        self.assertTrue(r["fallback"])

    def test_invalid_name_falls_back_to_default(self):
        r = self._resolve("bad name!!")
        self.assertEqual(r["name"], "default")

    def test_malformed_agents_json_still_yields_default(self):
        bad = os.path.join(self.dir, "bad.json")
        with open(bad, "w") as f:
            f.write("{not json")
        r = agents.resolve("trading", base=self.base, path=bad)
        self.assertEqual(r["name"], "default")
        self.assertTrue(r["api_url"].endswith("/v1/chat/completions"))


class AgentEntryTests(unittest.TestCase):
    """The shipped agents.json loads with the default + the live alternates (beepbop); describe()
    never leaks key material."""

    def setUp(self):
        self.entries = agents.load()

    def test_has_default_and_known_profiles(self):
        for name in ("default", "beepbop"):
            self.assertIn(name, self.entries)

    def test_default_is_the_sasshole_voice(self):
        self.assertEqual(self.entries["default"]["voice"], "sasshole")

    def test_every_shipped_profile_is_usable(self):
        unusable = [n for n, e in self.entries.items() if not e["usable"]]
        self.assertEqual(unusable, [])

    def test_describe_has_no_key_material(self):
        for a in agents.describe():
            self.assertNotIn("api_key", a)
            self.assertNotIn("personality_text", a)
            self.assertIn("label", a)
            self.assertIn("voice", a)

    def test_unknown_voice_in_entry_defaults_to_sasshole(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "agents.json")
            with open(p, "w") as f:
                json.dump({"default": {"voice": "shakespeare"}}, f)
            self.assertEqual(agents.load(p)["default"]["voice"], "sasshole")


class VoiceTests(unittest.TestCase):
    """system_prompt() swaps ONLY the persona line: the gadget rules are identical in every
    voice, and the sasshole voice is the historical system prompt verbatim."""

    RULES_MARKERS = ("one to three short spoken sentences", "[react:", "Moods:", "[image:", "DANGEROUS ACTIONS", "/approvals", "No markdown")

    def test_sasshole_is_the_historical_prompt_verbatim(self):
        self.assertEqual(relay.system_prompt("sasshole"), relay.SYSTEM)

    def test_default_argument_is_sasshole(self):
        self.assertEqual(relay.system_prompt(), relay.system_prompt("sasshole"))

    def test_professional_voice_differs_and_is_professional(self):
        p = relay.system_prompt("professional")
        self.assertNotEqual(p, relay.SYSTEM)
        self.assertIn("no-nonsense professional", p)
        self.assertNotIn("CUSS FREELY", p)

    def test_plain_voice_has_no_persona_line(self):
        p = relay.system_prompt("plain")
        self.assertEqual(p, relay.SYSTEM_INTRO + relay.SYSTEM_RULES)
        self.assertNotIn("Personality:", p)

    def test_gadget_rules_present_in_every_voice(self):
        for voice in ("sasshole", "professional", "plain"):
            p = relay.system_prompt(voice)
            self.assertTrue(p.startswith(relay.SYSTEM_INTRO))
            self.assertTrue(p.endswith(relay.SYSTEM_RULES))
            for marker in self.RULES_MARKERS:
                self.assertIn(marker, p, "voice %r missing %r" % (voice, marker))

    def test_unknown_voice_falls_back_to_sasshole_persona(self):
        self.assertEqual(relay.system_prompt("bogus"), relay.SYSTEM)


class _FakeHermes:
    """Records the route the relay dialed, so a turn's profile is assertable offline."""

    def __init__(self):
        self.calls = []

    def __call__(self, text, sid, route=None):
        self.calls.append(route or {})
        return {"choices": [{"message": {"content": "Sure thing."}}], "usage": {}}, "sid123"


class ProfileHttpTurnTests(unittest.TestCase):
    """End-to-end through the handler: ?profile= selects the /p/<name>/ route and voice; absent
    or unknown selects the default plain route. hermes_call is stubbed so nothing is dialed."""

    @classmethod
    def setUpClass(cls):
        import threading as _t
        cls.srv = relay.ThreadingHTTPServer(("127.0.0.1", 0), relay.H)
        cls.port = cls.srv.server_address[1]
        cls.t = _t.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.t.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def setUp(self):
        self.fake = _FakeHermes()
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self._sess = relay.SESSION_IDS_FILE
        relay.SESSION_IDS_FILE = os.path.join(self.dir, "session_ids.json")
        self.addCleanup(lambda: setattr(relay, "SESSION_IDS_FILE", self._sess))
        self.patches = [mock.patch.object(relay, "hermes_call", self.fake)]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)

    def _turn(self, query):
        import urllib.request
        req = urllib.request.Request(
            "http://127.0.0.1:%d/turn%s" % (self.port, query), method="POST", data=b"",
            headers={"X-Gadget-Token": relay.TOKEN})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read().decode())

    def test_default_turn_uses_the_plain_route(self):
        obj = self._turn("?text=hi")
        self.assertEqual(obj["heard"], "hi")
        route = self.fake.calls[-1]
        self.assertEqual(route["name"], "default")
        self.assertTrue(route["api_url"].endswith("/v1/chat/completions"))
        self.assertEqual(route["voice"], "sasshole")

    def test_profile_turn_uses_the_p_path_and_voice(self):
        if "beepbop" not in agents.load():
            self.skipTest("example profile beepbop not in agents.json")
        self._turn("?text=hi&profile=beepbop")
        route = self.fake.calls[-1]
        self.assertEqual(route["name"], "beepbop")
        self.assertIn("/p/beepbop/v1/chat/completions", route["api_url"])
        self.assertEqual(route["voice"], "plain")

    def test_unknown_profile_turn_routes_to_default(self):
        self._turn("?text=hi&profile=nope_nope")
        route = self.fake.calls[-1]
        self.assertEqual(route["name"], "default")
        self.assertTrue(route["fallback"])

    def test_empty_profile_is_default(self):
        self._turn("?text=hi&profile=")
        self.assertEqual(self.fake.calls[-1]["name"], "default")

    def test_sessions_are_per_agent(self):
        """Each agent slot keeps its own conversation; clearing one never touches the other."""
        relay.save_sid({"name": "default"}, "idA")
        relay.save_sid({"name": "beepbop"}, "idB")
        self.assertEqual(relay.load_sid({"name": "default"}), "idA")
        self.assertEqual(relay.load_sid({"name": "beepbop"}), "idB")
        relay.clear_sid({"name": "beepbop"})
        self.assertEqual(relay.load_sid({"name": "beepbop"}), "")
        self.assertEqual(relay.load_sid({"name": "default"}), "idA")


class _FakeResp:
    """Context-manager stand-in for urllib's response: iterable bytes lines + a headers map."""

    def __init__(self, lines):
        self._lines = list(lines)
        self.headers = {}

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def __iter__(self):
        return iter(self._lines)


class StreamRouteTests(unittest.TestCase):
    """The streaming path must dial the profile's URL/key and send the profile's system prompt."""

    def _capture(self, route):
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["url"] = req.full_url
            captured["auth"] = req.get_header("Authorization")
            captured["body"] = json.loads(req.data.decode())
            return _FakeResp([b'data: {"choices":[{"delta":{"content":"Hello."}}]}\n', b"data: [DONE]\n"])

        with mock.patch.object(relay.urllib.request, "urlopen", fake_urlopen):
            relay.hermes_call_stream_full("hi", "", lambda s: None, lambda t: None, route)
        return captured

    def test_default_stream_route(self):
        r = agents.resolve("", base="http://127.0.0.1:8642",
                           path=os.path.join(os.path.dirname(os.path.abspath(__file__)), "agents.json"))
        cap = self._capture(r)
        self.assertTrue(cap["url"].endswith("127.0.0.1:8642/v1/chat/completions"))
        self.assertEqual(cap["body"]["messages"][0]["content"], relay.SYSTEM)

    def test_named_stream_route_and_prompt(self):
        dir_ = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, dir_, ignore_errors=True)
        p = os.path.join(dir_, "agents.json")
        with open(p, "w") as f:
            json.dump({"default": {"voice": "sasshole"},
                       "beepbop": {"voice": "plain",
                                   "api_key_file": os.path.expanduser("~/.hermes/config.yaml")}}, f)
        r = agents.resolve("beepbop", base="http://127.0.0.1:8642", path=p)
        cap = self._capture(r)
        self.assertTrue(cap["url"].endswith("/p/beepbop/v1/chat/completions"))
        self.assertEqual(cap["auth"], "Bearer " + r["api_key"])
        self.assertEqual(cap["body"]["messages"][0]["content"], relay.system_prompt("plain"))


if __name__ == "__main__":
    unittest.main()


class HardeningTests(unittest.TestCase):
    """The socket/body/log guards added after the 2026-10-25 QA (N-M1/N-M2)."""

    def test_chunked_reader_refuses_oversize(self):
        body = b"5\r\nhello\r\n0\r\n\r\n"
        self.assertEqual(relay.read_chunked(io.BytesIO(body), cap=100), b"hello")
        with self.assertRaises(ValueError):
            relay.read_chunked(io.BytesIO(body), cap=3)

    def test_log_tee_timestamps_and_rotates(self):
        import os, tempfile, time as _t
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "relay.log")
            with open(p, "w") as f:
                f.write("x" * 50)
            tee = relay._LogTee(p, max_bytes=10)
            tee.write("hello\nworld\n")
            tee.flush()
            self.assertTrue(os.path.exists(p + ".1"), "old log rotated aside")
            lines = open(p).read().splitlines()
            self.assertEqual(len(lines), 2)
            self.assertRegex(lines[0], r"^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\] hello$")


class PacingAndPronunciationTests(unittest.TestCase):
    def test_spoken_markup_pauses(self):
        self.assertEqual(relay.spoken_markup("Done."), "Done. [pause short]")
        self.assertEqual(relay.spoken_markup("Wait..."), "Wait... [pause]")
        self.assertEqual(relay.spoken_markup("and then"), "and then [pause short]")

    def test_pronunciations_load_and_bad_file_is_ignored(self):
        import json, os, tempfile
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "pronunciations.json")
            with open(p, "w") as f:
                json.dump({"Etsy": "ˈɛtsi", "bad": 5}, f)
            old = relay.PRON_FILE
            relay.PRON_FILE = p
            try:
                got = relay.load_pronunciations()
            finally:
                relay.PRON_FILE = old
            self.assertEqual(got, {"pronunciations": [{"phrase": "Etsy", "phoneticEncoding":
                                    "PHONETIC_ENCODING_IPA", "pronunciation": "ˈɛtsi"}]})
