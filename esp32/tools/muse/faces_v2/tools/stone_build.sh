#!/bin/bash
# Stage B build for stone-B3: bake the sprite header + 52 mood clips + previews.
set -e
S=~/muse/art-pipeline/faces_v2/stage_b
cd $S/stone/pack
echo "=== sprite header (with the 21 pose tables) ==="
PYTHONPATH=. /opt/homebrew/bin/python3 make_stone_header.py ../sprite_stone.h
echo "=== 52 mood clips ==="
/opt/homebrew/bin/python3 bake_moods.py stone_b3 ../stone_moods.h
ls -la ../sprite_stone.h ../stone_moods.h
echo "=== stage into the preview tree ==="
cd $S/preview/esp32
cp $S/stone/sprite_stone.h $S/stone/stone_moods.h components/muse/avatar/
cp ~/muse/muse-gadget-sdk/esp32/components/muse/avatar/muse_pixel_stone.c components/muse/avatar/
echo "=== states+moods sheets (real renderer) ==="
/opt/homebrew/bin/python3 tools/muse/preview_sheets.py --src components/muse/avatar/muse_pixel_stone.c --out $S/sheets/stone --samples 8
echo "=== pose stills (real renderer) ==="
cc -O2 -w -I components/muse -I components/muse/avatar -DFACE_C='"muse_pixel_stone.c"' tools/muse/pose_dump.c -lm -o /tmp/pose_dump_stone
mkdir -p $S/preview/poses_stone
/tmp/pose_dump_stone $S/preview/poses_stone "alert,angry,bored,closed,confused_l,confused_r,disgust,evil,laugh,mouth_1,mouth_2,nervous_l,nervous_r,pleased,proud,sleepy,smirk,squint,surprised,target,wink" 2>&1 | tail -2
/opt/homebrew/bin/python3 $S/make_pose_sheet.py $S/preview/poses_stone $S/sheets/stone_poses.png $S/sheets/stone_poses.gif
echo STONE-BUILD-DONE
