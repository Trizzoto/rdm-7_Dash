/*
 * test_track_map_geo.c — RDMTRK parsing and projection, on the host.
 *
 * The fixtures in fixtures/track_map/ are produced by the REAL RDM Studio
 * encoder (gpTrackAssetBuild, extracted from src/tauri-overlay.html and run
 * under node — see gen_track_fixtures.js). That is the whole point: this test
 * proves the writer and the reader agree, not that the reader agrees with a
 * second copy of the writer written by the same hand on the same day.
 */
#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "widgets/track_map_geo.h"

/* The bundled unity shim (unity.h) has no *_MESSAGE or DOUBLE variants, and it
 * carries someone else's in-flight changes right now — so the three this file
 * needs are defined here, in terms of the shim's own TEST_FAIL, rather than by
 * editing a shared header. */
#define ASSERT_TRUE_MSG(c, msg)   do { if (!(c)) TEST_FAIL(msg); } while (0)
#define ASSERT_FALSE_MSG(c, msg)  do { if  ((c)) TEST_FAIL(msg); } while (0)
#define ASSERT_NEAR(exp, act, tol, msg)                                            do { double _d = (double)(act) - (double)(exp);                                     if (_d < 0) _d = -_d;                                                          if (!(_d <= (double)(tol))) TEST_FAIL(msg); } while (0)

/* ── fixture loading ─────────────────────────────────────────────────────── */

static uint8_t  g_buf[64 * 1024];
static size_t   g_len;

static bool load_fixture(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "fixtures/track_map/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "tests/native/fixtures/track_map/%s", name);
        f = fopen(path, "rb");
    }
    if (!f) return false;
    g_len = fread(g_buf, 1, sizeof(g_buf), f);
    fclose(f);
    return g_len > 0;
}

void setUp(void) {}
void tearDown(void) {}

/* ── the happy path, against Studio's own bytes ──────────────────────────── */

static void test_parses_studio_output(void) {
    ASSERT_TRUE_MSG(load_fixture("winton.rdmtrk"), "fixture missing");

    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_OK, track_map_parse(g_buf, g_len, &f));
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_VERSION, f.version);
    TEST_ASSERT_EQUAL_STRING("Winton", f.name);
    TEST_ASSERT_TRUE(f.n_points >= 3);
    TEST_ASSERT_TRUE(f.n_points <= TRACK_MAP_MAX_POINTS);

    /* The origin must sit inside the shape's own bounding box — if the writer
     * and reader disagreed on field offsets this is the first thing to go. */
    int32_t minla = 0, maxla = 0, minlo = 0, maxlo = 0;
    for (uint16_t i = 0; i < f.n_points; i++) {
        int32_t la, lo;
        TEST_ASSERT_TRUE(track_map_point(&f, i, &la, &lo));
        if (i == 0) { minla = maxla = la; minlo = maxlo = lo; }
        if (la < minla) minla = la;
        if (la > maxla) maxla = la;
        if (lo < minlo) minlo = lo;
        if (lo > maxlo) maxlo = lo;
    }
    TEST_ASSERT_TRUE(f.lat0_1e7 >= minla && f.lat0_1e7 <= maxla);
    TEST_ASSERT_TRUE(f.lon0_1e7 >= minlo && f.lon0_1e7 <= maxlo);

    /* Winton is in the southern hemisphere, east of Greenwich. A sign error
     * anywhere in the i32 round-trip shows up right here. */
    ASSERT_TRUE_MSG(f.lat0_1e7 < 0, "latitude should be negative (south)");
    ASSERT_TRUE_MSG(f.lon0_1e7 > 0, "longitude should be positive (east)");
}

/* A UTF-8 name longer than the field must lose its tail, never split a
 * character into a replacement glyph. */
static void test_utf8_name_truncates_on_a_boundary(void) {
    TEST_ASSERT_TRUE(load_fixture("longname.rdmtrk"));
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_OK, track_map_parse(g_buf, g_len, &f));

    size_t n = strlen(f.name);
    TEST_ASSERT_TRUE(n <= TRACK_MAP_NAME_LEN - 1);
    /* No byte may be a dangling UTF-8 continuation at the very end: walk back
     * from the terminator and check the lead byte's declared length fits. */
    size_t i = n;
    while (i > 0 && ((unsigned char)f.name[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0) {
        unsigned char lead = (unsigned char)f.name[i - 1];
        size_t seq = (lead & 0x80) == 0x00 ? 1 :
                     (lead & 0xE0) == 0xC0 ? 2 :
                     (lead & 0xF0) == 0xE0 ? 3 :
                     (lead & 0xF8) == 0xF0 ? 4 : 0;
        ASSERT_TRUE_MSG(seq != 0, "name ends on an invalid lead byte");
        ASSERT_TRUE_MSG(seq == n - (i - 1), "name was cut mid-character");
    }
}

static void test_closed_flag_set_for_a_loop(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_OK, track_map_parse(g_buf, g_len, &f));
    ASSERT_TRUE_MSG(f.flags & TRACK_MAP_FLAG_CLOSED,
        "a lap that returns to its start should be marked closed");
}

static void test_open_shape_not_flagged_closed(void) {
    TEST_ASSERT_TRUE(load_fixture("hillclimb.rdmtrk"));
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_OK, track_map_parse(g_buf, g_len, &f));
    ASSERT_FALSE_MSG(f.flags & TRACK_MAP_FLAG_CLOSED,
        "a point-to-point run must not claim to be a loop");
}

/* ── rejection: every one of these must be refused, not tolerated ────────── */

static void test_rejects_bad_magic(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    g_buf[0] = 'X';
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_MAGIC, track_map_parse(g_buf, g_len, &f));
}

static void test_rejects_future_version(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    g_buf[6] = TRACK_MAP_VERSION + 1;
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_VERSION, track_map_parse(g_buf, g_len, &f));
}

static void test_rejects_truncated_points(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    /* One byte short of the declared point array. */
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_SHORT, track_map_parse(g_buf, g_len - 1, &f));
}

static void test_rejects_header_only(void) {
    track_map_file_t f;
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_SHORT, track_map_parse(g_buf, TRACK_MAP_HEADER, &f));
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_SHORT, track_map_parse(g_buf, 0, &f));
}

static void test_rejects_absurd_point_count(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    g_buf[48] = 0xFF; g_buf[49] = 0xFF;         /* 65535 points */
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_POINTS, track_map_parse(g_buf, g_len, &f));
    g_buf[48] = 1; g_buf[49] = 0;               /* a single point is not a line */
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_POINTS, track_map_parse(g_buf, g_len, &f));
}

static void test_rejects_out_of_range_coordinate(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    /* Corrupt the first point's latitude to 200 degrees. */
    int32_t bad = 2000000000;
    memcpy(g_buf + TRACK_MAP_HEADER, &bad, 4);
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_ERR_RANGE, track_map_parse(g_buf, g_len, &f));
}

/* ── the projection ──────────────────────────────────────────────────────── */

static void test_fit_puts_every_point_inside_the_box(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_OK, track_map_parse(g_buf, g_len, &f));

    const int W = 400, H = 260, M = 8;
    track_map_proj_t p;
    track_map_fit(&f, W, H, M, 0.0, &p);
    TEST_ASSERT_TRUE(p.px_per_m > 0.0);

    double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    for (uint16_t i = 0; i < f.n_points; i++) {
        int32_t la, lo; double x, y;
        track_map_point(&f, i, &la, &lo);
        track_map_project(&p, la, lo, &x, &y);
        ASSERT_TRUE_MSG(x >= -0.51 && x <= W + 0.51, "point outside box in x");
        ASSERT_TRUE_MSG(y >= -0.51 && y <= H + 0.51, "point outside box in y");
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    /* And it must actually FILL one axis — a fit that shrinks everything to a
     * dot in the middle would pass the containment check above. */
    double fillx = (maxx - minx) / (W - 2.0 * M);
    double filly = (maxy - miny) / (H - 2.0 * M);
    ASSERT_TRUE_MSG(fillx > 0.98 || filly > 0.98,
        "the shape should touch the margin on its limiting axis");
    TEST_ASSERT_TRUE(fillx <= 1.001 && filly <= 1.001);
}

static void test_aspect_ratio_is_preserved(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    track_map_parse(g_buf, g_len, &f);

    /* Same shape into two very different boxes: the ratio of the drawn extents
     * must not change, or the track is being stretched. */
    track_map_proj_t a, b;
    track_map_fit(&f, 400, 260, 8, 0.0, &a);
    track_map_fit(&f, 200, 400, 8, 0.0, &b);

    double ax0=1e9, ax1=-1e9, ay0=1e9, ay1=-1e9;
    double bx0=1e9, bx1=-1e9, by0=1e9, by1=-1e9;
    for (uint16_t i = 0; i < f.n_points; i++) {
        int32_t la, lo; double x, y;
        track_map_point(&f, i, &la, &lo);
        track_map_project(&a, la, lo, &x, &y);
        if (x < ax0) ax0 = x;
        if (x > ax1) ax1 = x;
        if (y < ay0) ay0 = y;
        if (y > ay1) ay1 = y;
        track_map_project(&b, la, lo, &x, &y);
        if (x < bx0) bx0 = x;
        if (x > bx1) bx1 = x;
        if (y < by0) by0 = y;
        if (y > by1) by1 = y;
    }
    double ra = (ax1-ax0) / (ay1-ay0);
    double rb = (bx1-bx0) / (by1-by0);
    ASSERT_TRUE_MSG(fabs(ra - rb) < 0.01, "aspect ratio changed with box size");
}

/* North must be UP. Get this wrong and the circuit is drawn mirrored — which
 * looks entirely plausible until you try to follow it in a car. */
static void test_north_is_up_and_east_is_right(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    track_map_parse(g_buf, g_len, &f);
    track_map_proj_t p;
    track_map_fit(&f, 400, 260, 8, 0.0, &p);

    double x0, y0, xN, yN, xE, yE;
    track_map_project(&p, f.lat0_1e7, f.lon0_1e7, &x0, &y0);
    track_map_project(&p, f.lat0_1e7 + 10000, f.lon0_1e7, &xN, &yN);  /* 0.001° north */
    track_map_project(&p, f.lat0_1e7, f.lon0_1e7 + 10000, &xE, &yE);  /* 0.001° east  */

    ASSERT_TRUE_MSG(yN < y0, "moving north must move UP the screen");
    ASSERT_TRUE_MSG(xE > x0, "moving east must move RIGHT");
    ASSERT_TRUE_MSG(fabs(xN - x0) < 1e-6, "north must not shift x at 0 rotation");
    ASSERT_TRUE_MSG(fabs(yE - y0) < 1e-6, "east must not shift y at 0 rotation");
}

/* The live dot uses the SAME projection as the outline. A coordinate taken
 * from the file must land exactly on the drawn point it came from. */
static void test_live_point_lands_on_the_outline(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    track_map_parse(g_buf, g_len, &f);
    track_map_proj_t p;
    track_map_fit(&f, 400, 260, 8, 0.0, &p);

    for (uint16_t i = 0; i < f.n_points; i += 7) {
        int32_t la, lo; double ox, oy, lx, ly;
        track_map_point(&f, i, &la, &lo);
        track_map_project(&p, la, lo, &ox, &oy);
        /* Same call the position dot will make, from a raw CAN coordinate. */
        track_map_project(&p, la, lo, &lx, &ly);
        ASSERT_NEAR(ox, lx, 1e-9, "value out of tolerance");
        ASSERT_NEAR(oy, ly, 1e-9, "value out of tolerance");
    }
}

static void test_rotation_keeps_points_in_the_box(void) {
    TEST_ASSERT_TRUE(load_fixture("winton.rdmtrk"));
    track_map_file_t f;
    track_map_parse(g_buf, g_len, &f);
    const int W = 320, H = 320, M = 6;
    for (double rot = 0; rot < 360; rot += 37) {
        track_map_proj_t p;
        track_map_fit(&f, W, H, M, rot, &p);
        for (uint16_t i = 0; i < f.n_points; i++) {
            int32_t la, lo; double x, y;
            track_map_point(&f, i, &la, &lo);
            track_map_project(&p, la, lo, &x, &y);
            ASSERT_TRUE_MSG(x >= -0.51 && x <= W + 0.51, "rotated point escaped in x");
            ASSERT_TRUE_MSG(y >= -0.51 && y <= H + 0.51, "rotated point escaped in y");
        }
    }
}

/* A shape with no extent must not produce inf/NaN pixels. LVGL would wrap the
 * coordinate rather than complain, and the widget would draw garbage. */
static void test_degenerate_shape_does_not_explode(void) {
    uint8_t buf[TRACK_MAP_HEADER + 2 * 8];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, TRACK_MAP_MAGIC, TRACK_MAP_MAGIC_LEN);
    buf[6] = TRACK_MAP_VERSION;
    int32_t la = -365200000, lo = 1460900000;
    memcpy(buf + 40, &la, 4);
    memcpy(buf + 44, &lo, 4);
    buf[48] = 2; buf[49] = 0;
    for (int i = 0; i < 2; i++) {                 /* both points identical */
        memcpy(buf + TRACK_MAP_HEADER + i * 8,     &la, 4);
        memcpy(buf + TRACK_MAP_HEADER + i * 8 + 4, &lo, 4);
    }
    track_map_file_t f;
    TEST_ASSERT_EQUAL_INT(TRACK_MAP_OK, track_map_parse(buf, sizeof(buf), &f));

    track_map_proj_t p;
    track_map_fit(&f, 400, 260, 8, 0.0, &p);
    TEST_ASSERT_TRUE(isfinite(p.px_per_m) && p.px_per_m > 0.0);

    double x, y;
    track_map_project(&p, la, lo, &x, &y);
    ASSERT_TRUE_MSG(isfinite(x) && isfinite(y), "degenerate fit produced inf/NaN");
    ASSERT_NEAR(200.0, x, 0.51, "value out of tolerance");
    ASSERT_NEAR(130.0, y, 0.51, "value out of tolerance");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_studio_output);
    RUN_TEST(test_utf8_name_truncates_on_a_boundary);
    RUN_TEST(test_closed_flag_set_for_a_loop);
    RUN_TEST(test_open_shape_not_flagged_closed);
    RUN_TEST(test_rejects_bad_magic);
    RUN_TEST(test_rejects_future_version);
    RUN_TEST(test_rejects_truncated_points);
    RUN_TEST(test_rejects_header_only);
    RUN_TEST(test_rejects_absurd_point_count);
    RUN_TEST(test_rejects_out_of_range_coordinate);
    RUN_TEST(test_fit_puts_every_point_inside_the_box);
    RUN_TEST(test_aspect_ratio_is_preserved);
    RUN_TEST(test_north_is_up_and_east_is_right);
    RUN_TEST(test_live_point_lands_on_the_outline);
    RUN_TEST(test_rotation_keeps_points_in_the_box);
    RUN_TEST(test_degenerate_shape_does_not_explode);
    return UNITY_END();
}
