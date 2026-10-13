"""B3 obsidian-oracle face poses: Qwen-Image 2.1 edit + Consistency LoRA at the SAME small canvas as the base (384 px), seed 11.

Stage B copy of hermes-anim/gen_poses.py, re-pointed at the approved B3 art (base_b3_384.png,
the 6x nearest upscale of the approved 64 snap) and the B3 features.
"""
import os, sys, time
sys.path.insert(0, os.environ.get("COMFY_SKILL_DIR", "muse-comfyui-skill/scripts"))
import qwen_gadget as q

KEEP = ("Edit this pixel art portrait. Keep everything exactly as it is: the near-black obsidian face with its branching gold kintsugi crack veins, "
        "the dotted gold arc across the top of the head, the gold edging along the frame of the skull, the pale cream-yellow glowing eyes with their gold rims, "
        "the black background and the framing. Keep the same chunky pixel art style and the same limited colours. Change only this: ")
P = {
 "half":      "the eyelids come halfway down, so the glowing cream-yellow eyes are half closed, thin slits.",
 "closed":    "both eyes are fully closed: thin dark closed-eyelid lines instead of the glowing cream-yellow eyes.",
 "left":      "the glowing cream-yellow eyes look to the left side of the picture, pupils shifted left.",
 "right":     "the glowing cream-yellow eyes look to the right side of the picture, pupils shifted right.",
 "up":        "the glowing cream-yellow eyes look upward, pupils shifted up, eyelids slightly lowered.",
 "down":      "the glowing cream-yellow eyes look downward, pupils shifted down, eyelids lowered a little.",
 "wide":      "both glowing cream-yellow eyes are wide open and larger, with the eyebrows raised high. The eyes stay glowing cream-yellow almond shapes with no round pupils and no white of the eye.",
 "wink":      "the left eye of the picture is closed in a wink while the other eye stays open and glowing.",
 "smirk":     "one eyebrow is raised higher than the other, the eyes are half-lidded and glowing, and the mouth curls up into a smug one-sided smirk.",
 "squint":    "both eyes are narrowed to thin glowing slits in a suspicious look, with the eyebrows slightly lowered.",
 "surprised": "the eyebrows are raised high, both glowing cream-yellow eyes are wide open and larger, and the mouth is a small open oval. The eyes stay glowing cream-yellow almond shapes with no round pupils and no white of the eye.",
 "pleased":   "the eyes are soft curved happy arcs, the eyebrows are relaxed and slightly raised, and the mouth is a gentle pleased smile.",
 "angry":     "a furious look: the eyebrows are pulled low and sharply angled down toward the nose, the glowing eyes are narrowed and glaring, and the mouth is a tight frown.",
 "evil":      "a sinister look: the eyebrows are sharply angled down, the glowing eyes are narrow slits, and the mouth is a wide grin full of sharp pointed teeth.",
 "laugh":     "laughing: the eyes are squeezed into curved arcs and the mouth is wide open in a big laugh.",
 "sleepy":    "sleepy: the eyelids droop heavily so the eyes are nearly closed thin glowing slits, the brows are relaxed, the mouth is relaxed.",
 "confused_l":"confused: one eyebrow is raised high and the other lowered, the glowing eyes look to the left side of the picture, and the mouth is slightly crooked.",
 "confused_r":"confused: one eyebrow is raised high and the other lowered, the glowing eyes look to the right side of the picture, and the mouth is slightly crooked.",
 "disgust":   "disgusted: the nose is wrinkled, the eyes are narrowed, the eyebrows are lowered and pulled together, and the mouth is turned down in a grimace.",
 "nervous_l": "very nervous: both glowing eyes are much wider than normal and look to the left side of the picture, the eyebrows are raised high and slanted upward in the middle in a worried look, the mouth is a clear wavy zigzag line. The eyes stay glowing cream-yellow almond shapes with no round pupils and no white of the eye.",
 "nervous_r": "very nervous: both glowing eyes are much wider than normal and look to the right side of the picture, the eyebrows are raised high and slanted upward in the middle in a worried look, the mouth is a clear wavy zigzag line. The eyes stay glowing cream-yellow almond shapes with no round pupils and no white of the eye.",
 "proud":     "proud: the eyes are half closed in a content way, the eyebrows are slightly raised, and the mouth is a small satisfied smile.",
 "bored":     "bored: the eyelids are half lowered, the eyebrows are level, and the mouth is a flat straight line.",
 "target":    "focused: the glowing eyes are wide open and fixed straight ahead with intense glow, and the eyebrows are slightly lowered. The eyes stay glowing cream-yellow almond shapes with no round pupils and no white of the eye.",
 "alert":     "alarmed: the eyebrows are lowered sharply, the glowing eyes are wide open and intense, and the mouth is tense and tight. The eyes stay glowing cream-yellow almond shapes with no round pupils and no white of the eye.",
 "mouth_1":   "the mouth is slightly open, as if starting to speak, with a thin dark opening between the lips.",
 "mouth_2":   "the mouth is wide open, as if speaking loudly, showing a dark opening.",
}

if __name__ == "__main__":
    only = sys.argv[1].split(",") if len(sys.argv) > 1 and sys.argv[1] else list(P)
    res = int(sys.argv[2]) if len(sys.argv) > 2 else 384
    outdir = sys.argv[3] if len(sys.argv) > 3 else "poses_b3"
    os.makedirs(outdir, exist_ok=True)
    q.ensure_up()
    src = q.upload("base_b3_384.png", "hermes_base_b3_384.png")
    t0 = time.time()
    for n in only:
        out = f"{outdir}/pose_{n}.png"
        if os.path.exists(out):
            continue
        t = q.run(q.graph(KEEP + P[n], seed=11, size=res, src=src, lora=q.LORA_CONSISTENCY, prefix=f"hermes_pose_b3_{n}"), out)
        print(f"{n:11s} {t:5.1f}s  {os.popen('file -b ' + out).read().strip()[:40]}", flush=True)
