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
 * The board's real-time clock. The BSP claims the shared bus carries one but
 * names no part; the PMU's datasheet has no time registers (only RTCLDO power
 * for an external chip). >i2c.scan's probe reports ghost addresses on this bus,
 * so >rtc.probe reads the registers of the usual RTC addresses instead: a real
 * PCF85063 answers with BCD seconds/minutes/hours at 0x04..0x06.
 */

#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus);

/* Probes 0x08..0x77 and prints every responder (bench: >i2c.scan). */
void muse_rtc_scan(void);

/* Reads the register windows of the usual RTC parts (bench: >rtc.probe). */
void muse_rtc_probe(void);
