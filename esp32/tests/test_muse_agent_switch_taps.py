# SPDX-License-Identifier: Apache-2.0

"""The talk-button agent-switch gesture: TWO quick taps (was three).

Compiles the production count_tap() from muse_input.c against host stubs and drives a
fake clock, so the tap rules are pinned without hardware: a tap is a press under
300 ms, taps must land within 1200 ms of each other, and two of them switch the agent."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static (?:unsigned|void|int|bool) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class AgentSwitchTaps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        src = (ROOT / 'components/muse/muse_input.c').read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "tap-test"
#define ESP_LOGI(...) ((void)0)
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static TickType_t g_now;
static TickType_t xTaskGetTickCount(void) { return g_now; }

/* The guard reads the first byte: a non-empty string keeps the gesture enabled. */
#define CONFIG_MUSE_HERMES_URL "http://relay.test"

/* A stand-in for the settings agent slot: idx + the ALT profile name. */
#define MUSE_AGENT_MUSE 0
#define MUSE_AGENT_HERMES 1
#define MUSE_AGENT_HERMES_ALT 2
#define MUSE_AGENT_COUNT 3
#define MUSE_AGENT_PROFILE_MAX 32
static int g_agent = MUSE_AGENT_MUSE;
static char g_alt[64] = "";
static int muse_settings_agent(void) { return g_agent; }
/* Production semantics, modeled faithfully so a wrong getter FAILS here: agent_profile is
 * ACTIVE-ONLY (routing/status/task-stamping use it, and must never name a profile the turn
 * isn't targeting); agent_alt is the STORED name (the cycle gate uses it). A stub that returned
 * the stored name from agent_profile masked the live bug where the cycle read "" on Muse/Hermes
 * and silently skipped ALT. */
__attribute__((unused)) static void muse_settings_agent_profile(char *out) {
    strcpy(out, g_agent == MUSE_AGENT_HERMES_ALT ? g_alt : "");
}
__attribute__((unused)) static void muse_settings_agent_alt(char *out) { strcpy(out, g_alt); }
static void muse_settings_set_agent(int idx) { g_agent = idx; }

''' + function(src, 'count_tap') + r'''

static void tap(TickType_t t_press, TickType_t t_release) {
    g_now = t_press; count_tap(true);
    g_now = t_release; count_tap(false);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0:  /* two quick taps switch Muse -> Hermes; with no ALT profile the cycle
              * is the old two-way toggle, so two more taps come back to Muse */
        assert(g_agent == MUSE_AGENT_MUSE);
        tap(1000, 1050);
        assert(g_agent == MUSE_AGENT_MUSE);      /* one tap: nothing */
        tap(1200, 1250);
        assert(g_agent == MUSE_AGENT_HERMES);    /* two taps: switch */
        tap(1400, 1450);
        tap(1600, 1650);
        assert(g_agent == MUSE_AGENT_MUSE);      /* and back */
        break;
    case 1:  /* taps further apart than the window do not accumulate */
        tap(1000, 1050);
        tap(2600, 2650);                   /* 1550 ms after the first release */
        assert(g_agent == MUSE_AGENT_MUSE);
        break;
    case 2:  /* a long press is a talk press, not a tap */
        g_now = 1000; count_tap(true);
        g_now = 1500; count_tap(false);    /* 500 ms hold: resets the count */
        assert(g_agent == MUSE_AGENT_MUSE);
        tap(1700, 1750);                   /* one quick tap after it */
        assert(g_agent == MUSE_AGENT_MUSE);
        tap(1900, 1950);                   /* the second quick tap switches */
        assert(g_agent == MUSE_AGENT_HERMES);
        break;
    case 3:  /* WITH an ALT profile, two taps cycle Muse -> Hermes -> ALT -> Muse */
        strcpy(g_alt, "trading");
        g_agent = MUSE_AGENT_MUSE;
        tap(1000, 1050); tap(1200, 1250);
        assert(g_agent == MUSE_AGENT_HERMES);
        tap(1400, 1450); tap(1600, 1650);
        assert(g_agent == MUSE_AGENT_HERMES_ALT);
        tap(1800, 1850); tap(2000, 2050);
        assert(g_agent == MUSE_AGENT_MUSE);
        break;
    case 4:  /* WITHOUT a profile the ALT slot is skipped even from Hermes */
        strcpy(g_alt, "");
        g_agent = MUSE_AGENT_HERMES;
        tap(1000, 1050); tap(1200, 1250);
        assert(g_agent == MUSE_AGENT_MUSE);
        break;
    default: return 2;
    }
    return 0;
}
'''
        c = Path(cls.tmp.name) / 'taps.c'
        c.write_text(code)
        cls.binary = c.with_suffix('')
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(c), '-o', str(cls.binary)], check=True, capture_output=True)

    def test_two_quick_taps_switch_the_agent(self):
        subprocess.run([str(self.binary), '0'], check=True)

    def test_far_apart_taps_do_not_accumulate(self):
        subprocess.run([str(self.binary), '1'], check=True)

    def test_long_press_is_not_a_tap(self):
        subprocess.run([str(self.binary), '2'], check=True)

    def test_with_an_alt_profile_the_cycle_is_three_way(self):
        subprocess.run([str(self.binary), '3'], check=True)

    def test_without_a_profile_the_alt_slot_is_skipped(self):
        subprocess.run([str(self.binary), '4'], check=True)
