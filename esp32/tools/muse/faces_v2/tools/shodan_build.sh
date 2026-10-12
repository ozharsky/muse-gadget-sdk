#!/bin/bash
# Stage B build for shodan-A1: bake the pose library + previews.
set -e
S=~/muse/art-pipeline/faces_v2/stage_b
cd $S/shodan
echo "=== bake poses ==="
/opt/homebrew/bin/python3 make_shodan_poses_a1.py
ls -la shodan_poses.h sprite_data.h
echo "=== stage into the preview tree ==="
cd $S/preview/esp32
cp $S/shodan/sprite_data.h $S/shodan/shodan_poses.h components/muse/avatar/
cp ~/muse/muse-gadget-sdk/esp32/components/muse/avatar/muse_pixel.c components/muse/avatar/
echo "=== states+moods sheets (real renderer) ==="
/opt/homebrew/bin/python3 tools/muse/preview_sheets.py --src components/muse/avatar/muse_pixel.c --out $S/sheets/shodan --samples 8
echo "=== pose stills (real renderer) ==="
cc -O2 -w -I components/muse -I components/muse/avatar -DSHODAN_STYLE -DFACE_C='"muse_pixel.c"' tools/muse/pose_dump.c -lm -o /tmp/pose_dump_shodan
mkdir -p $S/preview/poses_shodan
/tmp/pose_dump_shodan $S/preview/poses_shodan "half,closed,left,right,up,down,wide,wink,target,smirk,squint,surprised,pleased,angry,evil,laugh,sleepy,confused_l,confused_r,disgust,nervous_l,nervous_r,proud,bored,alert,mouth_1,mouth_2" 2>&1 | tail -2
/opt/homebrew/bin/python3 $S/make_pose_sheet.py $S/preview/poses_shodan $S/sheets/shodan_poses.png $S/sheets/shodan_poses.gif
echo SHODAN-BUILD-DONE
