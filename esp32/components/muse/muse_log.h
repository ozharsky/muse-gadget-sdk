/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Logs that outlive the USB cable.
 *
 * Every ESP_LOG line is kept in a 32 KB RAM ring (>log.ram). Warnings and
 * errors also go to a flash ring in the "logs" partition: 63 blocks of 8 KB,
 * circular, ~500 KB of history that survives unplugging, a reboot or a dead
 * battery. >log.save copies the whole RAM ring to flash (call it before a
 * reboot, or when collecting a bug report), >log.dump prints the flash ring.
 *
 * Why not every line to flash: Wi-Fi logging alone would erase a block every
 * few seconds and eat the flash's endurance for noise. W/E is rare; the RAM
 * ring covers the rest and can be saved on demand or before an app restart.
 */

#pragma once

#include <stddef.h>

/* Installs the log hook and finds the "logs" partition. Call once, early. */
void muse_log_init(void);

/* Prints the flash ring (oldest first). kb = 0 or negative means all of it. */
void muse_log_dump(int kb);

/* Prints the RAM ring (everything since boot, oldest first). */
void muse_log_ram_dump(int kb);

/* Copies the RAM ring into the flash ring and flushes now. */
void muse_log_save(void);

/* Erases the flash ring. */
void muse_log_clear(void);
