/* test_ota_skip_version.c — host-side coverage of the OTA per-version
 * dismissal comparator.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * When the user hits "Skip This Version" in the auto-OTA dialog, the
 * offered version string is written to NVS namespace "ota_cfg" / key
 * "skip_ver". On every subsequent boot, after `check_for_update()`
 * resolves the upstream version, the dialog stays silent if the upstream
 * version still matches the stored skip. As soon as upstream releases
 * anything newer (different string) the stored value is stale and the
 * popup fires again.
 *
 * The comparator is one line:
 *
 *     if (latest && skip[0] && strcmp(latest, skip) == 0) { silent; }
 *     else                                                { show; }
 *
 * Tiny, but easy to get wrong (e.g. comparing a NULL latest, or treating
 * an unset skip as a wildcard match). The real call sits inside
 * _boot_check_task in main/net/ota_handler.c, which transitively pulls
 * in mbedTLS, lwIP, ESP-IDF logging, FreeRTOS, NVS, and LVGL async
 * dispatch. Here we mirror just the predicate as a pure helper and lock
 * the decision down. If the boot-check ever grows version semantics
 * (proper semver compare, regex, etc.) these tests need updating in
 * lockstep with the firmware.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/net/ota_handler.c, _boot_check_task() — Auto-OTA-check branch
 *   main/net/ota_update_dialog.c, skip_btn_event_cb() — write path
 *   main/storage/config_store.{c,h} — NVS load/save helpers + 32-byte
 *                                      version-string budget
 */
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ── Helper under test ─────────────────────────────────────────────────
 *
 * Mirrors the line inside _boot_check_task verbatim. Returns true if the
 * dialog should stay silent (skip matches the offered version), false
 * if it should fire.
 *
 * Convention from config_store_load_ota_skip_version: on first boot (or
 * after factory reset) the skip buffer is zero-initialised. An empty
 * string is the documented "no skip stored" sentinel and must never
 * match an actual upstream version. */
static bool ota_should_suppress(const char *latest, const char *skip) {
    if (!latest)    return false;   /* nothing offered → no suppression decision */
    if (!skip)      return false;   /* defensive — caller always passes a buffer */
    if (skip[0] == '\0') return false; /* unset skip never matches */
    return strcmp(latest, skip) == 0;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* First boot — never-stored skip. NVS load returns the buffer untouched
 * (zero-initialised by the caller). Popup must fire. */
static void test_unset_skip_never_matches(void) {
    char skip[32] = {0};
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10", skip));
}

/* Exact match — the user skipped this exact version, popup stays silent. */
static void test_exact_match_suppresses(void) {
    char skip[32] = "1.1.10";
    TEST_ASSERT_TRUE(ota_should_suppress("1.1.10", skip));
}

/* Stored skip is stale — upstream moved on. Popup must fire again. */
static void test_newer_version_overrides_skip(void) {
    char skip[32] = "1.1.10";
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.11", skip));
}

/* The opposite drift — upstream rolled back / republished older. Skip
 * string still doesn't match exactly, so the popup fires (we leave
 * "is x newer than y?" semantics out; the comparator is pure string
 * equality by design). */
static void test_rollback_below_skip_still_fires(void) {
    char skip[32] = "1.1.10";
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.9", skip));
}

/* Pre-release tag drift — semver-like strings with different suffixes are
 * different versions for the purposes of this comparator. */
static void test_prerelease_suffix_changes_match(void) {
    char skip[32] = "1.1.10-rc1";
    TEST_ASSERT_TRUE(ota_should_suppress("1.1.10-rc1", skip));
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10-rc2", skip));
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10", skip));
}

/* NULL upstream string (e.g. get_latest_version() failed mid-call —
 * shouldn't happen in practice when status == OTA_UPDATE_AVAILABLE, but
 * defensive). Decision: don't suppress on NULL — better to skip the
 * popup branch entirely in the caller than mis-suppress here. */
static void test_null_latest_does_not_suppress(void) {
    char skip[32] = "1.1.10";
    TEST_ASSERT_FALSE(ota_should_suppress(NULL, skip));
}

/* Empty upstream string — pathological but handled gracefully. The
 * comparator returns false because skip[0] == '\0' branch is checked
 * first only on the *skip* side; an empty `latest` against a non-empty
 * `skip` is just a regular non-match. */
static void test_empty_latest_does_not_match_nonempty_skip(void) {
    char skip[32] = "1.1.10";
    TEST_ASSERT_FALSE(ota_should_suppress("", skip));
}

/* Whitespace-only or "garbage" version strings — comparator is byte-
 * exact, so as long as both sides are byte-identical, suppression
 * triggers. Documents the strict-equality contract. */
static void test_byte_exact_match_for_garbage_strings(void) {
    char skip[32] = "  1.1.10  ";
    TEST_ASSERT_TRUE(ota_should_suppress("  1.1.10  ", skip));
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10",     skip));   /* trimmed */
}

/* Case sensitivity — comparator uses strcmp, not strcasecmp. The version
 * strings come from the OTA manifest verbatim and from the dialog's
 * cached `s_offered_version` (which was assigned from the same source),
 * so case must always agree exactly. */
static void test_case_sensitivity(void) {
    char skip[32] = "1.1.10-RC1";
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10-rc1", skip));
}

/* Maximum-length version strings fitting in the 32-byte buffer. Lock in
 * that the comparator doesn't get tripped up by long suffixes. */
static void test_long_version_string_within_buffer(void) {
    /* 31 chars + NUL — fills the buffer. */
    char skip[32] = "1.2.3-alpha.beta.gamma.20251231";
    TEST_ASSERT_TRUE(ota_should_suppress("1.2.3-alpha.beta.gamma.20251231", skip));
    /* Off-by-one differs at the very last char. */
    TEST_ASSERT_FALSE(ota_should_suppress("1.2.3-alpha.beta.gamma.20260101", skip));
}

/* Strings differing only in trailing newline (one of the more annoying
 * real-world drifts when version is read from a HTTP body without trim).
 * strcmp says these are different, popup fires. Documents that the
 * upstream parser is responsible for stripping line terminators. */
static void test_trailing_newline_breaks_match(void) {
    char skip[32] = "1.1.10";
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10\n", skip));
}

/* After a successful skip + reboot + new release lands, the buffer
 * still contains the OLD skip string until the user skips again. This
 * is the bug scenario that motivated the feature in the first place
 * (auto-popup harassment): verify the comparator doesn't accidentally
 * hide the new release. */
static void test_after_release_old_skip_does_not_hide_new(void) {
    char skip[32] = "1.1.9";  /* user skipped 1.1.9 last week */
    /* Upstream just shipped 1.1.10. */
    TEST_ASSERT_FALSE(ota_should_suppress("1.1.10", skip));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unset_skip_never_matches);
    RUN_TEST(test_exact_match_suppresses);
    RUN_TEST(test_newer_version_overrides_skip);
    RUN_TEST(test_rollback_below_skip_still_fires);
    RUN_TEST(test_prerelease_suffix_changes_match);
    RUN_TEST(test_null_latest_does_not_suppress);
    RUN_TEST(test_empty_latest_does_not_match_nonempty_skip);
    RUN_TEST(test_byte_exact_match_for_garbage_strings);
    RUN_TEST(test_case_sensitivity);
    RUN_TEST(test_long_version_string_within_buffer);
    RUN_TEST(test_trailing_newline_breaks_match);
    RUN_TEST(test_after_release_old_skip_does_not_hide_new);
    return UNITY_END();
}
