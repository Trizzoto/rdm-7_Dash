/* test_widget_decimals_follow_channel.c — channel decimals → widget precision.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * A channel owns its decimals; a panel / bar / text readout bound to it should
 * follow that unless the user set a per-widget override in Widget settings.
 *
 * The rule was only ever applied in from_json:
 *
 *     decimals_overridden = cfg carries "decimals"
 *     if (!decimals_overridden) pd->decimals = bound_c->decimals;
 *
 * `decimals_overridden` was a from_json LOCAL, so the channel-changed callback
 * had no way to know whether it was allowed to re-adopt — and didn't try. Two
 * consequences:
 *
 *   1. Changing decimals in the Channels editor did nothing on screen. The
 *      callback re-rendered, but with the widget's stale precision.
 *   2. Worse, it was sticky. to_json emits decimals only when it DIFFERS from
 *      the bound channel's, so the now-stale widget value looked like a
 *      deliberate override and got written into the layout. After that save the
 *      widget was pinned to the wrong precision for good — reloading no longer
 *      helped, because from_json saw an explicit override.
 *
 * The fix persists the override state on the widget (decimals_from_layout),
 * sets it when the inspector writes decimals, and re-adopts in the
 * channel-changed path when it isn't set.
 *
 * ── Test strategy ────────────────────────────────────────────────────────
 *
 * Same "mirror the firmware logic verbatim" approach as
 * test_widget_arc_precedence.c. The real callbacks repaint through LVGL; only
 * the decision is mirrored here. The mirror covers all three readout widgets at
 * once because they now share one rule — if they diverge again, these go red.
 *
 * Source-of-truth references (must stay in lockstep):
 *
 *   main/widgets/widget_panel.c  _panel_apply_channel / _panel_from_json /
 *                                _panel_to_json / _panel_inspector_set
 *   main/widgets/widget_bar.c    _bar_apply_channel / _bar_from_json /
 *                                _bar_to_json / _bar_inspector_set
 *   main/widgets/widget_text.c   _text_on_channel_changed / _text_from_json /
 *                                _text_to_json / _text_inspector_set
 */
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

/* ── Mirrors ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t decimals;
} chan_mirror_t;

typedef struct {
    const chan_mirror_t *channel;
    uint8_t decimals;
    bool    decimals_from_layout;
} widget_mirror_t;

/* The channel-changed path: adopt unless the widget carries its own override. */
static void widget_apply_channel(widget_mirror_t *w) {
    const chan_mirror_t *c = w->channel;
    if (!c) return;
    if (!w->decimals_from_layout) w->decimals = c->decimals;
}

/* Inspector write — an explicit per-widget choice pins the precision. */
static void widget_inspector_set_decimals(widget_mirror_t *w, uint8_t v) {
    w->decimals = v;
    w->decimals_from_layout = true;
}

/* to_json: emit decimals only as an OVERRIDE (differs from the bound channel).
 * has_key reports whether the saved layout carries the key at all. */
typedef struct {
    bool    has_decimals;
    uint8_t decimals;
} saved_cfg_t;

static saved_cfg_t widget_to_json(const widget_mirror_t *w) {
    saved_cfg_t out = {0};
    if (!w->channel || w->decimals != w->channel->decimals) {
        out.has_decimals = true;
        out.decimals = w->decimals;
    }
    return out;
}

/* from_json + channel bind, in that order (matches the firmware). */
static widget_mirror_t widget_from_json(const saved_cfg_t *cfg, const chan_mirror_t *c) {
    widget_mirror_t w = {0};
    w.decimals_from_layout = cfg->has_decimals;
    if (cfg->has_decimals) w.decimals = cfg->decimals;
    w.channel = c;
    if (c && !w.decimals_from_layout) w.decimals = c->decimals;
    return w;
}

/* ── Fixtures ────────────────────────────────────────────────────────────── */

/* A readout that has never been given a per-widget precision. */
static widget_mirror_t following_widget(const chan_mirror_t *c) {
    saved_cfg_t cfg = {0};              /* no "decimals" key in the layout */
    return widget_from_json(&cfg, c);
}

/* ── The regression ──────────────────────────────────────────────────────── */

static void test_channel_edit_reaches_the_widget(void) {
    chan_mirror_t c = { .decimals = 0 };
    widget_mirror_t w = following_widget(&c);
    TEST_ASSERT_EQUAL_INT(0, w.decimals);

    /* User picks 2 decimals in the Channels editor on the dash. */
    c.decimals = 2;
    widget_apply_channel(&w);

    TEST_ASSERT_EQUAL_INT(2, w.decimals);
}

static void test_channel_edit_does_not_get_baked_in_as_an_override(void) {
    /* The sticky half of the bug: a stale widget value looks like a deliberate
     * override to to_json, so the next save pins it permanently. */
    chan_mirror_t c = { .decimals = 0 };
    widget_mirror_t w = following_widget(&c);

    c.decimals = 2;
    widget_apply_channel(&w);

    saved_cfg_t saved = widget_to_json(&w);
    TEST_ASSERT_FALSE(saved.has_decimals);      /* nothing to override */

    widget_mirror_t back = widget_from_json(&saved, &c);
    TEST_ASSERT_FALSE(back.decimals_from_layout);
    TEST_ASSERT_EQUAL_INT(2, back.decimals);

    /* Still following after the round trip. */
    c.decimals = 3;
    widget_apply_channel(&back);
    TEST_ASSERT_EQUAL_INT(3, back.decimals);
}

static void test_repeated_notifies_are_stable(void) {
    chan_mirror_t c = { .decimals = 1 };
    widget_mirror_t w = following_widget(&c);
    for (int i = 0; i < 10; i++) widget_apply_channel(&w);
    TEST_ASSERT_EQUAL_INT(1, w.decimals);
}

/* ── The override must still win ─────────────────────────────────────────── */

static void test_layout_override_survives_a_channel_edit(void) {
    chan_mirror_t c = { .decimals = 0 };
    saved_cfg_t cfg = { .has_decimals = true, .decimals = 3 };
    widget_mirror_t w = widget_from_json(&cfg, &c);
    TEST_ASSERT_EQUAL_INT(3, w.decimals);

    c.decimals = 1;
    widget_apply_channel(&w);

    TEST_ASSERT_EQUAL_INT(3, w.decimals);   /* the user's choice is theirs */
}

static void test_inspector_edit_pins_the_widget(void) {
    /* Before the fix this was the silent trap: setting decimals in Widget
     * settings left decimals_from_layout unset, so the next channel notify
     * clobbered it. */
    chan_mirror_t c = { .decimals = 0 };
    widget_mirror_t w = following_widget(&c);

    widget_inspector_set_decimals(&w, 2);
    c.decimals = 0;
    widget_apply_channel(&w);

    TEST_ASSERT_EQUAL_INT(2, w.decimals);
    TEST_ASSERT_TRUE(w.decimals_from_layout);
}

static void test_inspector_override_round_trips(void) {
    chan_mirror_t c = { .decimals = 0 };
    widget_mirror_t w = following_widget(&c);
    widget_inspector_set_decimals(&w, 2);

    saved_cfg_t saved = widget_to_json(&w);
    TEST_ASSERT_TRUE(saved.has_decimals);
    TEST_ASSERT_EQUAL_INT(2, saved.decimals);

    widget_mirror_t back = widget_from_json(&saved, &c);
    TEST_ASSERT_TRUE(back.decimals_from_layout);
    TEST_ASSERT_EQUAL_INT(2, back.decimals);
}

/* ── Edges ───────────────────────────────────────────────────────────────── */

static void test_unbound_widget_keeps_its_own_decimals(void) {
    saved_cfg_t cfg = { .has_decimals = true, .decimals = 2 };
    widget_mirror_t w = widget_from_json(&cfg, NULL);
    widget_apply_channel(&w);               /* no channel — no-op */
    TEST_ASSERT_EQUAL_INT(2, w.decimals);

    /* An unbound widget always persists its precision — there's no channel to
     * fall back to on reload. */
    saved_cfg_t saved = widget_to_json(&w);
    TEST_ASSERT_TRUE(saved.has_decimals);
}

static void test_override_equal_to_channel_is_not_persisted(void) {
    /* Pre-existing, deliberate: an override that matches the channel is
     * indistinguishable from following it, so it reloads as following. Locked
     * in so the behaviour is a choice rather than a surprise. */
    chan_mirror_t c = { .decimals = 2 };
    widget_mirror_t w = following_widget(&c);
    widget_inspector_set_decimals(&w, 2);

    saved_cfg_t saved = widget_to_json(&w);
    TEST_ASSERT_FALSE(saved.has_decimals);

    widget_mirror_t back = widget_from_json(&saved, &c);
    TEST_ASSERT_FALSE(back.decimals_from_layout);
}

static void test_zero_decimals_is_a_real_override_not_absent(void) {
    /* 0 is a legitimate override value; the flag has to come from key PRESENCE,
     * not from the value being non-zero. */
    chan_mirror_t c = { .decimals = 2 };
    saved_cfg_t cfg = { .has_decimals = true, .decimals = 0 };
    widget_mirror_t w = widget_from_json(&cfg, &c);

    TEST_ASSERT_TRUE(w.decimals_from_layout);
    TEST_ASSERT_EQUAL_INT(0, w.decimals);

    c.decimals = 3;
    widget_apply_channel(&w);
    TEST_ASSERT_EQUAL_INT(0, w.decimals);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* the regression */
    RUN_TEST(test_channel_edit_reaches_the_widget);
    RUN_TEST(test_channel_edit_does_not_get_baked_in_as_an_override);
    RUN_TEST(test_repeated_notifies_are_stable);

    /* overrides still win */
    RUN_TEST(test_layout_override_survives_a_channel_edit);
    RUN_TEST(test_inspector_edit_pins_the_widget);
    RUN_TEST(test_inspector_override_round_trips);

    /* edges */
    RUN_TEST(test_unbound_widget_keeps_its_own_decimals);
    RUN_TEST(test_override_equal_to_channel_is_not_persisted);
    RUN_TEST(test_zero_decimals_is_a_real_override_not_absent);

    return UNITY_END();
}
