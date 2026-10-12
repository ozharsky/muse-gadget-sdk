"""The five eye-only poses the main gen_poses.py does not emit (they were hand-packed in the original).

    run from the shodan pack dir; uses the SAME base/seed/LoRA as gen_poses.py.
"""
import os, sys, time
sys.path.insert(0, "/Users/olegzharsky/Comfy UI/muse-comfyui-skill/scripts")
import qwen_gadget as q

KEEP = ("Edit this pixel art portrait. Keep everything exactly as it is: the tall pale silver android face with its smooth bald head and grey shading, the big dark green-grey eye sockets, "
        "the bright green stripe running down the centre of the forehead, the green circuit traces on the near-black background, the black background and the framing. "
        "Keep the same chunky pixel art style and the same limited colours. Change only this: ")
P = {
 "half":   "the eyelids come halfway down, so the glowing green eyes are half closed, thin slits.",
 "closed": "both eyes are fully closed: thin dark closed-eyelid lines instead of the glowing green eyes.",
 "left":   "the glowing green eyes look to the left side of the picture, pupils shifted left.",
 "right":  "the glowing green eyes look to the right side of the picture, pupils shifted right.",
 "wink":   "the left eye of the picture is closed in a wink while the other eye stays open and glowing.",
}

if __name__ == "__main__":
    os.makedirs("poses_a1", exist_ok=True)
    q.ensure_up()
    src = q.upload("base_a1.png", "gadget_base_a1.png")
    t0 = time.time()
    for n in P:
        out = f"poses_a1/pose_{n}.png"
        if os.path.exists(out):
            continue
        t = q.run(q.graph(KEEP + P[n], seed=11, src=src, lora=q.LORA_CONSISTENCY, prefix=f"shodan_pose_{n}"), out)
        print(f"{n:11s} {t:5.1f}s  (total {time.time()-t0:5.0f}s)", flush=True)
    print("EXTRA5 DONE", flush=True)
