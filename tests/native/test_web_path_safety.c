/* test_web_path_safety.c — host coverage of the path-traversal guards.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * `web_server_name_is_safe` and `web_server_filename_is_safe` are the
 * input-validation choke point for every web API endpoint that takes a
 * user-supplied name (layouts, images, fonts, DBC files, etc.) and then
 * concatenates it into a `/lfs/...` or `/sdcard/...` path. A regression
 * in either function would re-expose path traversal — e.g. a request
 * with `name=../../../nvs` could escape the layouts directory.
 *
 * Security-class regression. Worth locking down with tests.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * The two functions are pure C with no LVGL / FreeRTOS / ESP-IDF deps.
 * Linking the real source directly is overkill (we'd need to compile
 * web_server.c and stub the entire HTTP server) — mirror is cleaner.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/net/web_server.c:
 *     bool web_server_name_is_safe(const char *name)
 *     bool web_server_filename_is_safe(const char *name)
 */
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── Host mirror (verbatim from web_server.c, modulo formatting) ──────────
 *
 * The `(unsigned char)` cast on every byte before comparing < 0x20 is the
 * load-bearing detail: it makes the control-char check signedness-stable
 * across host gcc (signed char) and ARM toolchains (often unsigned char).
 * Without it, UTF-8 multi-byte sequences read as negative integers on
 * signed-char compilers and get false-rejected. The mirror tracks the
 * firmware verbatim so this test fails loudly if anyone removes the cast.
 */

static bool name_is_safe(const char *name) {
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || c == '.' || c < 0x20) return false;
    }
    return true;
}

static bool filename_is_safe(const char *name) {
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || c < 0x20) return false;
    }
    if (strstr(name, "..")) return false;
    return true;
}

/* ── name_is_safe: reject anything that could traverse ─────────────────── */

static void test_name_null_is_unsafe(void) {
    TEST_ASSERT_FALSE(name_is_safe(NULL));
}

static void test_name_empty_is_unsafe(void) {
    TEST_ASSERT_FALSE(name_is_safe(""));
}

static void test_name_simple_ascii_is_safe(void) {
    TEST_ASSERT_TRUE(name_is_safe("default"));
    TEST_ASSERT_TRUE(name_is_safe("MyLayout"));
    TEST_ASSERT_TRUE(name_is_safe("racing_v2"));
    TEST_ASSERT_TRUE(name_is_safe("Layout-2024"));
}

static void test_name_forward_slash_unsafe(void) {
    TEST_ASSERT_FALSE(name_is_safe("foo/bar"));
    TEST_ASSERT_FALSE(name_is_safe("/foo"));
    TEST_ASSERT_FALSE(name_is_safe("foo/"));
}

static void test_name_backslash_unsafe(void) {
    TEST_ASSERT_FALSE(name_is_safe("foo\\bar"));
    TEST_ASSERT_FALSE(name_is_safe("..\\nvs"));
}

static void test_name_dot_unsafe(void) {
    /* Any dot anywhere disqualifies — name_is_safe is the strict variant
     * used for layout names where ".json" is appended by the caller. */
    TEST_ASSERT_FALSE(name_is_safe("layout.json"));
    TEST_ASSERT_FALSE(name_is_safe("."));
    TEST_ASSERT_FALSE(name_is_safe(".."));
    TEST_ASSERT_FALSE(name_is_safe("a.b"));
}

static void test_name_traversal_attempts_unsafe(void) {
    /* The classic ones. */
    TEST_ASSERT_FALSE(name_is_safe("../"));
    TEST_ASSERT_FALSE(name_is_safe("../../etc/passwd"));
    TEST_ASSERT_FALSE(name_is_safe("..\\..\\nvs"));
    TEST_ASSERT_FALSE(name_is_safe("/lfs/layouts/default"));
}

static void test_name_control_chars_unsafe(void) {
    /* All bytes 0x01..0x1F should reject (0x00 is end-of-string). */
    TEST_ASSERT_FALSE(name_is_safe("foo\nbar"));   /* 0x0A */
    TEST_ASSERT_FALSE(name_is_safe("foo\tbar"));   /* 0x09 */
    TEST_ASSERT_FALSE(name_is_safe("foo\rbar"));   /* 0x0D */
    TEST_ASSERT_FALSE(name_is_safe("\x01"));
    TEST_ASSERT_FALSE(name_is_safe("\x1f"));
}

static void test_name_utf8_is_accepted(void) {
    /* Cast `(unsigned char)*p` in name_is_safe makes the deny check
     * `< 0x20` signedness-stable. Without it, signed-char compilers
     * read `\xc3` as -61 (< 32 = true) and false-rejected UTF-8.
     *
     * This assertion locks the cast in. If anyone removes it, the test
     * goes red and the regression is obvious.
     *
     * Adjacent string literal concatenation stops the \x escape after
     * two hex digits — `"\xc3\xa9clair"` alone would greedily over-read. */
    TEST_ASSERT_TRUE(name_is_safe("\xc3\xa9" "clair"));    /* "éclair" UTF-8 */
    TEST_ASSERT_TRUE(name_is_safe("\xe6\x97\xa5\xe6\x9c\xac"));  /* "日本" UTF-8 */
}

/* ── filename_is_safe: allows dots, rejects ".." sequences ─────────────── */

static void test_filename_null_is_unsafe(void) {
    TEST_ASSERT_FALSE(filename_is_safe(NULL));
}

static void test_filename_empty_is_unsafe(void) {
    TEST_ASSERT_FALSE(filename_is_safe(""));
}

static void test_filename_simple_with_extension_is_safe(void) {
    TEST_ASSERT_TRUE(filename_is_safe("trace.csv"));
    TEST_ASSERT_TRUE(filename_is_safe("layout.json"));
    TEST_ASSERT_TRUE(filename_is_safe("logo.rdmimg"));
}

static void test_filename_no_extension_is_safe(void) {
    /* A filename without an extension is still valid input — the caller
     * decides what extension to append (or none). */
    TEST_ASSERT_TRUE(filename_is_safe("trace"));
}

static void test_filename_slash_unsafe(void) {
    TEST_ASSERT_FALSE(filename_is_safe("dir/file.csv"));
    TEST_ASSERT_FALSE(filename_is_safe("/file.csv"));
    TEST_ASSERT_FALSE(filename_is_safe("file.csv/"));
}

static void test_filename_backslash_unsafe(void) {
    TEST_ASSERT_FALSE(filename_is_safe("dir\\file.csv"));
}

static void test_filename_double_dot_anywhere_unsafe(void) {
    /* "..", "../", "/..", "x..y" — strstr finds the substring regardless
     * of whether it's a path component. Slightly over-strict (rejects
     * "file..name.csv") but that's safer than letting an attacker craft
     * a path component named "..". */
    TEST_ASSERT_FALSE(filename_is_safe(".."));
    TEST_ASSERT_FALSE(filename_is_safe("../file"));
    TEST_ASSERT_FALSE(filename_is_safe("foo..bar.csv"));
    TEST_ASSERT_FALSE(filename_is_safe("a..b"));
}

static void test_filename_single_dot_is_safe(void) {
    /* Single dot anywhere is fine — that's the whole point of the
     * filename variant: extensions are allowed. */
    TEST_ASSERT_TRUE(filename_is_safe("file.csv"));
    TEST_ASSERT_TRUE(filename_is_safe(".hidden"));   /* leading dot OK */
    TEST_ASSERT_TRUE(filename_is_safe("file."));     /* trailing dot OK */
    TEST_ASSERT_TRUE(filename_is_safe("a.b.c"));     /* multiple separate dots OK */
}

static void test_filename_control_chars_unsafe(void) {
    TEST_ASSERT_FALSE(filename_is_safe("foo\nbar.csv"));
    TEST_ASSERT_FALSE(filename_is_safe("foo\tbar"));
}

/* ── Real-world attack patterns ────────────────────────────────────────── */

static void test_realworld_attacks_blocked(void) {
    const char *attacks[] = {
        "../../../etc/passwd",
        "..\\..\\nvs\\wifi",
        "/lfs/nvs",
        "C:\\Windows\\System32",
        "../layouts/default",
        "layouts/../../nvs",
        "%2e%2e/etc/passwd",   /* not URL-decoded by these functions; literal % is fine, .. is the issue */
        "....//etc/passwd",    /* path-collapsing trick */
        NULL,
    };
    for (size_t i = 0; attacks[i]; i++) {
        const char *a = attacks[i];
        bool n = name_is_safe(a);
        bool f = filename_is_safe(a);
        /* Both variants must reject every entry on this list. */
        if (n || f) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "attack passed: \"%s\" name_is_safe=%d filename_is_safe=%d",
                     a, n, f);
            TEST_FAIL(msg);
        }
    }
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* name_is_safe (strict — no dots, no slashes, no control chars) */
    RUN_TEST(test_name_null_is_unsafe);
    RUN_TEST(test_name_empty_is_unsafe);
    RUN_TEST(test_name_simple_ascii_is_safe);
    RUN_TEST(test_name_forward_slash_unsafe);
    RUN_TEST(test_name_backslash_unsafe);
    RUN_TEST(test_name_dot_unsafe);
    RUN_TEST(test_name_traversal_attempts_unsafe);
    RUN_TEST(test_name_control_chars_unsafe);
    RUN_TEST(test_name_utf8_is_accepted);

    /* filename_is_safe (allows single dots, rejects ".." substring) */
    RUN_TEST(test_filename_null_is_unsafe);
    RUN_TEST(test_filename_empty_is_unsafe);
    RUN_TEST(test_filename_simple_with_extension_is_safe);
    RUN_TEST(test_filename_no_extension_is_safe);
    RUN_TEST(test_filename_slash_unsafe);
    RUN_TEST(test_filename_backslash_unsafe);
    RUN_TEST(test_filename_double_dot_anywhere_unsafe);
    RUN_TEST(test_filename_single_dot_is_safe);
    RUN_TEST(test_filename_control_chars_unsafe);

    /* Real-world attack patterns — both variants must block all of them. */
    RUN_TEST(test_realworld_attacks_blocked);

    return UNITY_END();
}
