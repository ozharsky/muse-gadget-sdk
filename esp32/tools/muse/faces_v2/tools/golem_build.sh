#!/bin/bash
# Stage B build for golem-C5: bake the sprite header + 52 mood clips + previews.
set -e
S=~/muse/art-pipeline/faces_v2/stage_b
G=$S/golem
PK=$G/pack
cd $PK
echo "=== sprite header ==="
/opt/homebrew/bin/python3 make_golem_header.py $G/sprite_golem.h
echo "=== 52 mood clips ==="
/opt/homebrew/bin/python3 bake_moods.py golem $G/golem_moods.h
ls -la $G/sprite_golem.h $G/golem_moods.h
echo "=== stage into the preview tree (header + moods + patched face C) ==="
cd $S/preview/esp32
cp $G/sprite_golem.h $G/golem_moods.h components/muse/avatar/
cp ~/muse/muse-gadget-sdk/esp32/components/muse/avatar/muse_pixel_golem.c components/muse/avatar/
/opt/homebrew/bin/python3 $PK/patch_preview_golem.py components/muse/avatar/muse_pixel_golem.c
echo "=== states+moods sheets (real renderer) ==="
/opt/homebrew/bin/python3 tools/muse/preview_sheets.py --src components/muse/avatar/muse_pixel_golem.c --out $S/sheets/golem --samples 8
echo GOLEM-BUILD-DONE
