# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The two agents have independent transports; the UI must not conflate them.

Hermes talks to the user's Mac relay, Muse to Meta's cloud. A Muse outage used to read as
"Can't connect" for the whole device even while Hermes kept working (verified on hardware:
a full Hermes turn ran with Muse offline). These contract checks lock in that:

  * the firmware exposes a Hermes-only link state (so the UI can show it separately),
  * the Hermes turn path feeds that state on both success and failure,
  * boot does not report a device-level failure for a Muse-only fault while the active
    agent is Hermes.
"""
from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHAT_SESSION = ROOT / "components" / "muse" / "muse_chat_session.cpp"
CHAT_H = ROOT / "components" / "muse" / "muse_chat.h"
APP_C = ROOT / "main" / "app.c"
SETTINGS_UI = ROOT / "components" / "muse" / "muse_settings_ui.c"


class AgentIndependenceContractTest(unittest.TestCase):
    def test_hermes_link_state_is_exposed(self) -> None:
        header = CHAT_H.read_text()
        self.assertIn("int muse_hatch_hermes_link(void);", header)
        self.assertIn("void muse_hatch_hermes_link_note(bool ok);", header)

    def test_hermes_turn_path_records_both_outcomes(self) -> None:
        source = CHAT_SESSION.read_text()
        # Success: a reply arrived from the relay.
        self.assertIn("muse_hatch_hermes_link_note(true)", source)
        # Failure: the exchange ended with no reply.
        self.assertIn("muse_hatch_hermes_link_note(false)", source)

    def test_hermes_link_is_scoped_to_the_active_agent(self) -> None:
        source = CHAT_SESSION.read_text()
        body = source[source.index("extern \"C\" int muse_hatch_hermes_link(void)"):]
        body = body[: body.index("}")]
        # Not configured, or Hermes is not the active agent -> "no signal", not a failure.
        # "Configured" is now runtime (compiled-in OR saved from the setup page).
        self.assertIn("muse_settings_relay_configured()", body)
        self.assertIn("muse_settings_agent_hermes()", body)

    def test_boot_does_not_fail_the_device_for_a_muse_only_fault_on_hermes(self) -> None:
        source = APP_C.read_text()
        # Find the branch that stays up, then confirm its CONDITION checks the active
        # agent (searching back past the explanatory comment to the `} else if (...)`).
        start = source.index('ui_set_status("hermes_only")')
        guard = source[max(0, start - 1200):start]
        self.assertIn("muse_settings_agent_hermes()", guard)
        self.assertIn("muse_settings_relay_configured()", guard)

    def test_settings_show_each_link_on_its_own_row(self) -> None:
        source = SETTINGS_UI.read_text()
        # A dedicated Hermes row, separate from the Muse row.
        self.assertIn('"Hermes", &s_home_hermes', source)
        self.assertIn('"Muse", &s_home_hatch', source)
        self.assertIn("muse_hatch_hermes_link()", source)


if __name__ == "__main__":
    unittest.main()
