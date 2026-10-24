#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* The Glance page (a tile right of the face): clock + rich cards from the relay's /glance. */
void muse_glance_ui_build(lv_obj_t *tile);
/* Display lock held. visible = the page is on the glass (opening it fetches fresh cards);
 * settled = the pages aren't moving (new cards wait for that). */
void muse_glance_ui_tick(bool visible, bool settled);
void muse_glance_ui_bench_scroll(int y);   /* bench: ">glance.scroll=PX" */
