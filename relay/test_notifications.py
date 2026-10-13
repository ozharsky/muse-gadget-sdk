#!/usr/bin/env python3
"""Tests for the gadget's reverse notification channel: notifications.py + relay endpoints.

The load-bearing property is IDENTITY: a notification must carry -- and the device must open --
the EXACT agent that raised it, never a generic handler and never the wrong agent. These pin:

  * resolve_agent maps a profile name to the right SLOT (default Hermes vs ALT) and falls back
    safely (unknown/unusable -> the default slot, so the device can always open something);
  * a stored notification round-trips through add/pending/ack with its identity intact;
  * the relay's /notifications endpoints serve and ack, token-gated.

Run:  python3 -m unittest test_notifications
"""
import json
import os
import tempfile
import time
import unittest

import agents
import notifications


class NotifyIdentity(unittest.TestCase):
    def test_default_and_empty_resolve_to_the_hermes_slot(self):
        for name in ("", "default", None):
            who = notifications.resolve_agent(name)
            self.assertEqual(who["agent"], agents.DEFAULT_NAME)
            self.assertTrue(who["is_default"], "empty/default must open the Hermes slot")

    def test_a_known_profile_resolves_to_the_alt_slot_with_its_name(self):
        if not (agents.resolve("sidekick").get("usable") if hasattr(agents, "resolve") else True):
            self.skipTest("edit agents.json for this machine first (example profile: sidekick)")
        who = notifications.resolve_agent("sidekick")
        self.assertEqual(who["agent"], "sidekick")
        self.assertFalse(who["is_default"], "a real profile must open the ALT slot, not default")
        self.assertEqual(who["label"], "Sidekick")

    def test_unknown_profile_falls_back_to_default_never_an_unroutable_name(self):
        who = notifications.resolve_agent("no_such_profile_xyz")
        self.assertEqual(who["agent"], agents.DEFAULT_NAME)
        self.assertTrue(who["is_default"])
        self.assertEqual(who["requested"], "no_such_profile_xyz")   # recorded, but not opened


class NotifyStore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self._orig = notifications.STORE
        notifications.STORE = os.path.join(self.tmp, "notifications.json")

    def tearDown(self):
        notifications.STORE = self._orig

    def test_add_round_trips_identity_and_acks_once(self):
        if not (agents.resolve("sidekick").get("usable") if hasattr(agents, "resolve") else True):
            self.skipTest("edit agents.json for this machine first (example profile: sidekick)")
        it = notifications.add("build finished", agent="sidekick", mood="alert")
        self.assertEqual(it["agent"], "sidekick")
        self.assertFalse(it["is_default"])
        pend = notifications.pending()
        self.assertEqual(len(pend), 1)
        self.assertEqual(pend[0]["id"], it["id"])
        self.assertTrue(notifications.ack(it["id"]))
        self.assertEqual(notifications.pending(), [], "acked nudge must not present again")
        self.assertFalse(notifications.ack(it["id"]), "a second ack finds nothing")

    def test_newest_first(self):
        a = notifications.add("one", agent="default")
        b = notifications.add("two", agent="sidekick")
        order = [n["id"] for n in notifications.pending()]
        self.assertEqual(order, [b["id"], a["id"]])

    def test_empty_text_is_refused(self):
        self.assertIsNone(notifications.add("   ", agent="sidekick"))
        self.assertEqual(notifications.pending(), [])

    def test_corrupt_store_reads_as_empty_never_raises(self):
        with open(notifications.STORE, "w") as f:
            f.write("{ this is not json")
        self.assertEqual(notifications.pending(), [])


class NotifyHistory(unittest.TestCase):
    """The inbox: history keeps read items (a missed nudge must still be findable), and
    cancel DELETES (a cancelled reminder must never fire)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self._orig = notifications.STORE
        notifications.STORE = os.path.join(self.tmp, "notifications.json")

    def tearDown(self):
        notifications.STORE = self._orig

    def test_history_keeps_read_items_newest_first(self):
        a = notifications.add("one", agent="default")
        b = notifications.add("two", agent="sidekick")
        notifications.ack(a["id"])
        h = notifications.history()
        self.assertEqual([n["id"] for n in h], [b["id"], a["id"]],
                         "an acked nudge stays in history; only pending() hides it")
        self.assertTrue([n for n in h if n["id"] == a["id"]][0]["read"])

    def test_history_limit(self):
        for i in range(5):
            notifications.add("n%d" % i, agent="default")
        self.assertEqual(len(notifications.history(2)), 2)

    def test_cancel_deletes_outright(self):
        it = notifications.add("remind me", agent="default", due=time.time() + 3600)
        self.assertEqual(len(notifications.history()), 1, "an un-fired reminder is in history")
        self.assertTrue(notifications.cancel(it["id"]))
        self.assertEqual(notifications.history(), [], "a cancelled reminder must not linger")
        self.assertFalse(notifications.cancel(it["id"]), "a second cancel finds nothing")

    def test_cancel_is_not_ack(self):
        it = notifications.add("still coming", agent="default", due=time.time() + 3600)
        notifications.ack(it["id"])
        self.assertEqual(len(notifications.history()), 1, "ack = presented, row stays")
        notifications.cancel(it["id"])
        self.assertEqual(len(notifications.history()), 0, "cancel = gone")

    def test_clear_keeps_unfired_reminders(self):
        """The gadget's Clear: history goes, commitments stay."""
        notifications.add("a past nudge", agent="default")
        notifications.add("shown already", agent="sidekick")
        notifications.ack(notifications.history()[0]["id"])
        keep = notifications.add("tea in 10", agent="default", due=time.time() + 600)
        fired = notifications.add("went off", agent="default", due=time.time() - 5)
        kept = notifications.clear_history()
        rows = notifications.history()
        self.assertEqual(kept, 1, "one un-fired reminder survives")
        self.assertEqual([n["id"] for n in rows], [keep["id"]], "only the un-fired reminder stays")
        self.assertNotIn(fired["id"], [n["id"] for n in rows], "a fired reminder is history now")

    def test_clear_history_on_an_empty_store_is_safe(self):
        self.assertEqual(notifications.clear_history(), 0)


class RelayEndpoints(unittest.TestCase):
    """Exercise the real relay endpoints over HTTP against a throwaway notification store."""

    @classmethod
    def setUpClass(cls):
        import relay
        import threading
        import urllib.request
        cls.relay = relay
        cls.tmp = tempfile.mkdtemp()
        cls._orig = notifications.STORE
        notifications.STORE = os.path.join(cls.tmp, "notifications.json")
        notifications.clear()
        # A tiny server on an ephemeral port, same handler as production.
        from http.server import ThreadingHTTPServer
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), relay.H)
        cls.port = cls.httpd.server_address[1]
        cls.t = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.t.start()
        cls.token = relay.TOKEN

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        notifications.STORE = cls._orig

    def _req(self, path, method="GET", body=None, token=True):
        import urllib.request
        url = "http://127.0.0.1:%d%s" % (self.port, path)
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        if token:
            req.add_header("X-Gadget-Token", self.token)
        if data:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())

    def test_create_then_serve_then_ack(self):
        if not (agents.resolve("sidekick").get("usable") if hasattr(agents, "resolve") else True):
            self.skipTest("edit agents.json for this machine first (example profile: sidekick)")
        notifications.clear()
        out = self._req("/notifications", "POST",
                        {"text": "the print finished", "agent": "sidekick", "mood": "alert"})
        self.assertTrue(out["ok"])
        got = out["notification"]
        self.assertEqual(got["agent"], "sidekick")
        self.assertFalse(got["is_default"])
        served = self._req("/notifications")["notifications"]
        self.assertEqual(len(served), 1)
        self.assertEqual(served[0]["agent"], "sidekick")   # the EXACT agent, round-tripped
        ack = self._req("/notifications/ack", "POST", {"id": got["id"]})
        self.assertTrue(ack["ok"])
        self.assertEqual(self._req("/notifications")["notifications"], [])

    def test_missing_text_is_400(self):
        import urllib.error
        with self.assertRaises(urllib.error.HTTPError) as cm:
            self._req("/notifications", "POST", {"agent": "sidekick"})
        self.assertEqual(cm.exception.code, 400)

    def test_no_token_is_401(self):
        import urllib.error
        with self.assertRaises(urllib.error.HTTPError) as cm:
            self._req("/notifications", token=False)
        self.assertEqual(cm.exception.code, 401)

    def test_history_endpoint_includes_read_items(self):
        notifications.clear()
        out = self._req("/notifications", "POST", {"text": "old news", "agent": "default"})
        nid = out["notification"]["id"]
        self._req("/notifications/ack", "POST", {"id": nid})
        h = self._req("/notifications/history")["notifications"]
        self.assertEqual(len(h), 1, "the inbox shows what was already presented")
        self.assertTrue(h[0]["read"])

    def test_history_limit_param(self):
        notifications.clear()
        for i in range(4):
            self._req("/notifications", "POST", {"text": "n%d" % i, "agent": "default"})
        self.assertEqual(len(self._req("/notifications/history?limit=2")["notifications"]), 2)

    def test_cancel_endpoint_deletes_and_reports(self):
        notifications.clear()
        out = self._req("/notifications", "POST",
                        {"text": "tea", "agent": "sidekick", "due_in": 3600})
        nid = out["notification"]["id"]
        self.assertEqual(self._req("/notifications")["notifications"], [],
                         "an un-fired reminder is not pending")
        self.assertTrue(self._req("/notifications/cancel", "POST", {"id": nid})["ok"])
        self.assertEqual(self._req("/notifications/history")["notifications"], [])
        self.assertFalse(self._req("/notifications/cancel", "POST", {"id": nid})["ok"],
                         "cancelling a gone id reports not-found")

    def test_clear_endpoint_keeps_unfired_reminders(self):
        notifications.clear()
        self._req("/notifications", "POST", {"text": "past news", "agent": "default"})
        self._req("/notifications", "POST", {"text": "tea", "agent": "default", "due_in": 3600})
        out = self._req("/notifications/clear", "POST", {})
        self.assertTrue(out["ok"])
        self.assertEqual(out["kept"], 1, "the endpoint reports what survived")
        h = self._req("/notifications/history")["notifications"]
        self.assertEqual([n["text"] for n in h], ["tea"])


if __name__ == "__main__":
    unittest.main()
