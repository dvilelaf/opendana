/* test_video.c - unit tests for the video viewport geometry. */
#include "test.h"
#include "opendana/ext/video.h"

#include <string.h>

static void check_rect(int win_w, int win_h, int integer, int aspect,
                       int x, int y, int w, int h)
{
    od_video_rect r;
    memset(&r, 0xA5, sizeof r);
    od_video_viewport(win_w, win_h, integer, aspect, &r);
    CHECK_EQ_U32(r.x, x);
    CHECK_EQ_U32(r.y, y);
    CHECK_EQ_U32(r.w, w);
    CHECK_EQ_U32(r.h, h);
}

static void test_video_square_integer_case(void)
{
    /* Exact multiples are used whole, centred, with black borders. */
    check_rect(768, 720, 1, 0, 0, 0, 768, 720);      /* 3x, exact        */
    check_rect(800, 600, 1, 0, 144, 60, 512, 480);   /* 2x, bars         */
    check_rect(1000, 1000, 1, 0, 116, 140, 768, 720);/* 3x on both axes  */
    check_rect(1920, 1080, 1, 0, 448, 60, 1024, 960);
    check_rect(300, 260, 1, 0, 22, 10, 256, 240);    /* 1x               */
}

static void test_video_aspect87_integer_case(void)
{
    /* 8:7 widens the frame: width = round(256*k*8/7), height = 240*k. */
    check_rect(320, 260, 1, 1, 13, 10, 293, 240);    /* k=1 -> 293      */
    check_rect(800, 600, 1, 1, 107, 60, 585, 480);   /* k=2 -> 585      */
    check_rect(640, 480, 1, 1, 27, 0, 585, 480);
    check_rect(1920, 1080, 1, 1, 375, 60, 1170, 960);/* k=4 -> 1170     */
}

static void test_video_scaled_width_case(void)
{
    CHECK_EQ_U32(od_video_scaled_width(1, 0), 256);
    CHECK_EQ_U32(od_video_scaled_width(2, 0), 512);
    CHECK_EQ_U32(od_video_scaled_width(3, 0), 768);
    CHECK_EQ_U32(od_video_scaled_width(1, 1), 293);
    CHECK_EQ_U32(od_video_scaled_width(2, 1), 585);
    CHECK_EQ_U32(od_video_scaled_width(3, 1), 878);
    CHECK_EQ_U32(od_video_scaled_width(4, 1), 1170);
    CHECK_EQ_U32(od_video_scaled_width(5, 1), 1463);
}

static void test_video_fit_case(void)
{
    /* Integer scaling off: largest size that keeps the aspect ratio. */
    check_rect(800, 600, 0, 0, 80, 0, 640, 600);
    check_rect(1000, 300, 0, 0, 340, 0, 320, 300);
    check_rect(512, 384, 0, 1, 21, 0, 469, 384);     /* 8:7 fill         */
    /* A window smaller than one whole multiple degrades to the fit so the
     * frame is still fully visible. */
    check_rect(100, 100, 1, 0, 0, 3, 99, 93);
    check_rect(1, 1, 1, 0, 0, 0, 1, 1);
}

static void test_video_degenerate_case(void)
{
    od_video_rect r;
    /* A zero or negative window is clamped to one pixel, never crashes. */
    od_video_viewport(0, 0, 1, 0, &r);
    CHECK(r.w >= 1 && r.h >= 1 && r.x == 0 && r.y == 0);
    od_video_viewport(-4, -4, 0, 1, &r);
    CHECK(r.w >= 1 && r.h >= 1);
    /* NULL is a no-op. */
    od_video_viewport(100, 100, 1, 1, NULL);
}

OD_TEST(test_video_square_integer_case)
OD_TEST(test_video_aspect87_integer_case)
OD_TEST(test_video_scaled_width_case)
OD_TEST(test_video_fit_case)
OD_TEST(test_video_degenerate_case)
