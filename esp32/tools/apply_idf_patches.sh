#!/bin/bash
# Apply/check the two upstream ESP-IDF SPI bus-lock fixes (v6.0.1 ships both bugs).
#
# WHY: both freeze the 1.43C screen:
#   1. spi_bus_lock.c bg_exit_core never resumes a waiting lock owner once BG is
#      fully finished -> lcd_send sleeps in dev_wait(portMAX_DELAY) forever while
#      LVGL waits on the flush.
#      (esp-idf 94831e429ba, "spi buslock multi dev acq/release logic issue")
#   2. TOCTOU race on lock->acquiring_dev between the completion ISR and
#      acquire_end_core -> LoadProhibited on the queued esp_lcd path.
#      (esp-idf a16c00376b8, issue #18527 -- third reproducer was stock
#       esp_lcd SPI panel + LVGL on ESP32-S3, same as this board.)
#
# Idempotent: checks fix signatures first; only applies when missing. Exits
# nonzero only when a patch neither applied nor was already in. Re-run after
# any IDF re-clone/checkout; a fresh IDF tree loses the fixes.
set -u
IDF="${IDF_PATH:-$HOME/esp/esp-idf-v6}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCHES="$HERE/idf-patches"
TARGET="$IDF/components/esp_hw_support/spi_bus_lock.c"

if [ ! -f "$TARGET" ]; then
    echo "idf check: $TARGET not found (IDF_PATH=$IDF) -- skipping"
    exit 0
fi

sig1="A waiting lock owner must be selected once BG is fully finished"
sig2="acquiring_dev = (spi_bus_lock_dev_t \*)lock->acquiring_dev"

if grep -q "$sig1" "$TARGET" && grep -q "$sig2" "$TARGET"; then
    echo "idf check: spi bus-lock fixes present"
    exit 0
fi

for p in "$PATCHES"/0001-spi-buslock-waiting-lock-owner.patch "$PATCHES"/0002-spi-buslock-toctou-race.patch; do
    [ -f "$p" ] || { echo "idf check: MISSING patch file $p"; exit 1; }
    if git -C "$IDF" apply --check "$p" 2>/dev/null; then
        git -C "$IDF" apply "$p" && echo "idf check: applied $(basename "$p")"
    elif git -C "$IDF" apply --reverse --check "$p" 2>/dev/null; then
        echo "idf check: $(basename "$p") already applied"
    else
        echo "idf check: FAILED to apply or verify $(basename "$p") -- fix by hand: $TARGET"
        exit 1
    fi
done

if ! grep -q "$sig1" "$TARGET" || ! grep -q "$sig2" "$TARGET"; then
    echo "idf check: patches ran but the fix signatures are still missing -- screen freezes WILL recur"
    exit 1
fi
echo "idf check: patches applied"
