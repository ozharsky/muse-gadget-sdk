"""Generate the SHODAN face-pose library with Qwen-Image 2.1 (edit + Consistency LoRA), seed 11, via the tunnel on :8189."""
import os, sys, time, json
sys.path.insert(0, os.environ.get("COMFY_SKILL_DIR", "muse-comfyui-skill/scripts"))
import qwen_gadget as q
KEEP = ("Edit this pixel art portrait. Keep everything exactly as it is: the tall pale silver android face with its smooth bald head and grey shading, the big dark green-grey eye sockets, "
        "the bright green stripe running down the centre of the forehead, the green circuit traces on the near-black background, the black background and the framing. "
        "Keep the same chunky pixel art style and the same limited colours. Change only this: ")
P = {
 "up":        "the glowing green eyes look upward, pupils shifted up, eyelids slightly lowered.",
 "down":      "the glowing green eyes look downward, pupils shifted down, eyelids lowered a little.",
 "wide":      "both glowing green eyes are wide open and larger, with the eyebrows raised high.",
 "smirk":     "one eyebrow is raised higher than the other, the eyes are half-lidded and glowing, and the mouth curls up into a smug one-sided smirk.",
 "squint":    "both eyes are narrowed to thin glowing slits in a suspicious look, with the eyebrows slightly lowered.",
 "surprised": "the eyebrows are raised high, both glowing green eyes are wide open and larger, and the mouth is a small open oval.",
 "pleased":   "the eyes are soft curved happy arcs, the eyebrows are relaxed and slightly raised, and the mouth is a gentle pleased smile.",
 "angry":     "a furious look: the eyebrows are pulled low and sharply angled down toward the nose, the glowing eyes are narrowed and glaring, and the mouth is a tight frown.",
 "evil":      "a sinister look: the eyebrows are sharply angled down, the glowing eyes are narrow slits, and the mouth is a wide grin full of sharp pointed teeth.",
 "laugh":     "laughing: the eyes are squeezed into curved arcs and the mouth is wide open in a big laugh.",
 "sleepy":    "sleepy: the eyelids droop heavily so the eyes are nearly closed thin glowing slits, the brows are relaxed, the mouth is relaxed.",
 "confused_l":"confused: one eyebrow is raised high and the other lowered, the glowing eyes look to the left side of the picture, and the mouth is slightly crooked.",
 "confused_r":"confused: one eyebrow is raised high and the other lowered, the glowing eyes look to the right side of the picture, and the mouth is slightly crooked.",
 "disgust":   "disgusted: the nose is wrinkled, the eyes are narrowed, the eyebrows are lowered and pulled together, and the mouth is turned down in a grimace.",
 "nervous_l": "very nervous: both glowing eyes are much wider than normal and look to the left side of the picture, the eyebrows are raised high and slanted upward in the middle in a worried look, the mouth is a clear wavy zigzag line, and there is a bead of sweat on the temple.",
 "nervous_r": "very nervous: both glowing eyes are much wider than normal and look to the right side of the picture, the eyebrows are raised high and slanted upward in the middle in a worried look, the mouth is a clear wavy zigzag line, and there is a bead of sweat on the temple.",
 "proud":     "proud: the eyes are half closed in a content way, the eyebrows are slightly raised, and the mouth is a small satisfied smile.",
 "bored":     "bored: the eyelids are half lowered, the eyebrows are level, and the mouth is a flat straight line.",
 "target":    "focused: the glowing eyes are wide open and fixed straight ahead with intense glow, and the eyebrows are slightly lowered.",
 "alert":     "alarmed: the eyebrows are lowered sharply, the glowing eyes are wide open and intense, and the mouth is tense and tight.",
 "mouth_1":   "the mouth is slightly open, as if starting to speak, with a thin dark opening between the lips.",
 "mouth_2":   "the mouth is wide open, as if speaking loudly, showing a dark opening.",
}
only = sys.argv[1].split(",") if len(sys.argv) > 1 else list(P)
q.ensure_up()
src = q.upload("base_a1.png", "gadget_base_a1.png")
t0 = time.time()
for n in only:
    os.makedirs("poses_a1", exist_ok=True)
    out = f"poses_a1/pose_{n}.png"
    if os.path.exists(out): continue
    t = q.run(q.graph(KEEP + P[n], seed=11, src=src, lora=q.LORA_CONSISTENCY, prefix=f"shodan_pose_{n}"), out)
    print(f"{n:12s} {t:5.1f}s  (total {time.time()-t0:5.0f}s)", flush=True)
print("DONE", flush=True)
