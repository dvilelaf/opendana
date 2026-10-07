/* HUD model: the status-bar clock, its one-frame latch and the ROUND / TIME
 * text formatting. Synthetic states only; no content and no SDL. */
#include "test.h"

#include "opendana/hud.h"

#include <stdio.h>
#include <string.h>

static void test_hud_eu_clock(void)
{
    /* The countdown reaches zero: the seconds digits go from 16 to 17
     * and the countdown reloads to 60. */
    od_hud_clock clock = {1, 0, 0, 0x16};
    CHECK_EQ_U32(od_hud_clock_tick(&clock, 0, 0), 1);
    CHECK_EQ_U32(clock.countdown, 60);
    CHECK_EQ_U32(clock.seconds, 0x17);
    od_hud_clock saved = clock;
    CHECK_EQ_U32(od_hud_clock_tick(&clock, 1, 0), 0);
    CHECK_EQ_U32(od_hud_clock_tick(&clock, 0, 0x20), 0);
    CHECK_EQ_MEM(&clock, &saved, sizeof(clock));
    for (int i = 0; i < 59; i++)
        od_hud_clock_tick(&clock, 0, 0);
    CHECK_EQ_U32(clock.seconds, 0x17);
    od_hud_clock_tick(&clock, 0, 0);
    CHECK_EQ_U32(clock.seconds, 0x18);
    clock = (od_hud_clock){1, 0x09, 0x59, 0x59};
    od_hud_clock_tick(&clock, 0, 0);
    char text[16];
    od_hud_clock_text(text, sizeof(text), &clock);
    CHECK_EQ_MEM(text, "10:00:00", 9);
    clock = (od_hud_clock){1, 0x99, 0x59, 0x59};
    CHECK_EQ_U32(od_hud_clock_tick(&clock, 0, 0), 0);
    CHECK_EQ_U32(clock.countdown, 0x80);
    saved = clock;
    CHECK_EQ_U32(od_hud_clock_tick(&clock, 0, 0), 1);
    CHECK_EQ_MEM(&clock, &saved, sizeof(clock));
    od_hud_clock_text(text, sizeof(text), &clock);
    CHECK_EQ_MEM(text, "99:59:59", 9);
}

OD_TEST(test_hud_eu_clock)

static void test_hud_queue(void)
{
    od_hud_clock clock = {1, 0, 0, 0x16};
    od_hud_latch latch = {.visible = clock};
    od_hud_begin_frame(&latch);
    CHECK_EQ_U32(latch.visible.seconds, 0x16);
    if (od_hud_clock_tick(&clock, 0, 0)) od_hud_queue(&latch, &clock);
    CHECK_EQ_U32(latch.visible.seconds, 0x16);
    CHECK_EQ_U32(latch.queued.seconds, 0x17);
    od_hud_begin_frame(&latch);
    CHECK_EQ_U32(latch.visible.seconds, 0x17);
    CHECK_EQ_U32(latch.pending, 0);
    if (od_hud_clock_tick(&clock, 0, 0x20)) od_hud_queue(&latch, &clock);
    CHECK_EQ_U32(latch.pending, 0);
    od_hud_begin_frame(&latch);
    CHECK_EQ_U32(latch.visible.seconds, 0x17);
}

OD_TEST(test_hud_queue)

static void test_hud_format(void)
{
    printf("- hud: ROUND / TIME text formatting\n");
    char buf[16];
    od_hud_round_text(buf, sizeof(buf), 0);
    CHECK(!strcmp(buf, "1-1"));
    od_hud_round_text(buf, sizeof(buf), 9);
    CHECK(!strcmp(buf, "1-10"));
    od_hud_round_text(buf, sizeof(buf), 10);
    CHECK(!strcmp(buf, "2-1"));
    od_hud_round_text(buf, sizeof(buf), 99);
    CHECK(!strcmp(buf, "10-10"));
    od_hud_round_text(buf, sizeof(buf), 100);
    CHECK(!strcmp(buf, "11-1"));
    od_hud_round_text(buf, sizeof(buf), 149);
    CHECK(!strcmp(buf, "15-10"));

    od_hud_time_text(buf, sizeof(buf), 0);
    CHECK(!strcmp(buf, "00:00:00"));
    od_hud_time_text(buf, sizeof(buf), 59);
    CHECK(!strcmp(buf, "00:00:00"));
    od_hud_time_text(buf, sizeof(buf), 60);
    CHECK(!strcmp(buf, "00:00:01"));
    od_hud_time_text(buf, sizeof(buf), 960);
    CHECK(!strcmp(buf, "00:00:16"));
    od_hud_time_text(buf, sizeof(buf), 3600);
    CHECK(!strcmp(buf, "00:01:00"));
    od_hud_time_text(buf, sizeof(buf), 360000);
    CHECK(!strcmp(buf, "01:40:00"));
    od_hud_time_text(buf, sizeof(buf), 21600000);
    CHECK(!strcmp(buf, "99:59:59"));
}

OD_TEST(test_hud_format)
