#!/usr/bin/env python3
"""Register a Hermes profile as a gadget agent — the one command for "I made a new profile".

    python3 new_agent.py <profile> [--label NAME] [--voice sasshole|professional|plain]

It adds/updates the profile's entry in agents.json (voice defaults to professional, label to
"Hermes <Profile>"). The profile must already exist (`hermes profile create <name>`), and its
config.yaml must carry an api_server key — the multiplexed gateway refuses a profile without
one, so the script checks and tells you exactly what is missing rather than silently adding a
dead entry.

After this: flash-free. The gadget reaches the profile by name; the relay picks it up on the
next read (agents.json is re-read per request). Set it on the gadget with `>agent.profile=<name>`
or Settings -> Agent (its label shows on the row) and double-tap cycles to it.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AGENTS = os.path.join(HERE, "agents.json")

# Same regex relay.py uses for the nested api_server key.
KEY_RE = re.compile(r"api_server:\s*\n(?:.*\n)*?\s+key: (\S+)")


def profile_config(name):
    if name == "default":
        return os.path.expanduser("~/.hermes/config.yaml")
    return os.path.expanduser("~/.hermes/profiles/%s/config.yaml" % name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[2])
    ap.add_argument("profile")
    ap.add_argument("--label")
    ap.add_argument("--voice", default="professional", choices=("sasshole", "professional", "plain"))
    a = ap.parse_args()

    name = a.profile.strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", name):
        sys.exit("bad profile name %r: use letters, digits, _ or - (the relay routes it in a URL)" % name)

    cfg = profile_config(name)
    if not os.path.exists(cfg):
        sys.exit("no profile %r (%s does not exist). Create it first: hermes profile create %s"
                 % (name, cfg, name))
    if not KEY_RE.search(open(cfg).read()):
        sys.exit("profile %r has no api_server key in %s.\n"
                 "The multiplexed gateway cannot route to it without one — add a\n"
                 "platforms: api_server: extra: key: block to that config (see the other profiles)."
                 % (name, cfg))

    try:
        data = json.load(open(AGENTS))
    except (OSError, ValueError) as e:
        sys.exit("cannot read %s: %s" % (AGENTS, e))

    personality = None
    soul = os.path.expanduser("~/.hermes/profiles/%s/SOUL.md" % name) if name != "default" else None
    if soul and os.path.exists(soul):
        personality = soul

    data[name] = {
        "label": a.label or ("Hermes" if name == "default" else "Hermes " + name.replace("_", " ").title()),
        "voice": a.voice,
        "api_key_file": cfg.replace(os.path.expanduser("~"), "~"),
        "personality_file": (personality.replace(os.path.expanduser("~"), "~") if personality else None),
    }
    json.dump(data, open(AGENTS, "w"), indent=2)
    print("registered agent %r (voice=%s) in %s" % (name, a.voice, AGENTS))
    print("on the gadget:  >agent.profile=%s  then  >agent=alt" % name)


if __name__ == "__main__":
    main()
