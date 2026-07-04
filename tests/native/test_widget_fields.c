/* test_widget_fields.c — widget_field_is_fractional() regression coverage.
 *
 * widget_field_is_fractional() is a pure, header-only predicate (widget_
 * fields.h) with no LVGL/FreeRTOS/ESP-IDF behaviour of its own — it just
 * compares two struct fields. widget_types.h pulls in <lvgl.h> and
 * <cJSON.h> to declare unrelated types, so we use the same mocks/ shims
 * as test_widget_rules.c to satisfy that include without linking real
 * LVGL or cJSON. No firmware .c file is compiled or linked — only the
 * struct layout and the inline function's logic are under test.
 *
 * This predicate's bug (commit 1990da9) truncated fractional field
 * defaults like 0.5/0.8 to 0 across four layers of the inspector before
 * the fix — a one-line regression with no prior coverage.
 */
#include "unity.h"

#include "../../main/widgets/widget_fields.h"

static widget_field_t _field(int32_t default_int, float default_float)
{
    widget_field_t f = {0};
    f.default_int = default_int;
    f.default_float = default_float;
    return f;
}

static void test_null_field_is_not_fractional(void)
{
    TEST_ASSERT_FALSE(widget_field_is_fractional(NULL));
}

static void test_integer_default_is_not_fractional(void)
{
    /* Codegen mirrors every numeric default into default_float, so a
     * genuinely-integer field still has default_float == (float)default_int. */
    widget_field_t f = _field(5, 5.0f);
    TEST_ASSERT_FALSE(widget_field_is_fractional(&f));
}

static void test_zero_default_is_not_fractional(void)
{
    widget_field_t f = _field(0, 0.0f);
    TEST_ASSERT_FALSE(widget_field_is_fractional(&f));
}

static void test_fractional_default_point_five_is_fractional(void)
{
    /* The exact regression case from commit 1990da9: a 0.5 threshold
     * field truncating to 0 in the pathbar shape inspector. */
    widget_field_t f = _field(0, 0.5f);
    TEST_ASSERT_TRUE(widget_field_is_fractional(&f));
}

static void test_fractional_default_point_eight_is_fractional(void)
{
    widget_field_t f = _field(0, 0.8f);
    TEST_ASSERT_TRUE(widget_field_is_fractional(&f));
}

static void test_negative_fractional_default_is_fractional(void)
{
    widget_field_t f = _field(0, -0.5f);
    TEST_ASSERT_TRUE(widget_field_is_fractional(&f));
}

static void test_fractional_default_with_nonzero_int_part_is_fractional(void)
{
    /* default_int truncates 1.5 to 1 — the discriminator must still catch it. */
    widget_field_t f = _field(1, 1.5f);
    TEST_ASSERT_TRUE(widget_field_is_fractional(&f));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_null_field_is_not_fractional);
    RUN_TEST(test_integer_default_is_not_fractional);
    RUN_TEST(test_zero_default_is_not_fractional);
    RUN_TEST(test_fractional_default_point_five_is_fractional);
    RUN_TEST(test_fractional_default_point_eight_is_fractional);
    RUN_TEST(test_negative_fractional_default_is_fractional);
    RUN_TEST(test_fractional_default_with_nonzero_int_part_is_fractional);

    return UNITY_END();
}
