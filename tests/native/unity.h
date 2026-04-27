/* Tiny Unity-style test framework — single header, ~70 lines.
 *
 * Not the real ThrowTheSwitch/Unity (that's a multi-file release with config
 * options we don't need). This is a stripped-down equivalent that gives us:
 *   - A test runner with PASS/FAIL/total counts.
 *   - The most-used asserts: TRUE, FALSE, EQUAL_INT, EQUAL_INT64,
 *     EQUAL_UINT, EQUAL_HEX, EQUAL_STRING, EQUAL_FLOAT, NULL, NOT_NULL.
 *   - Exit code 0 on success, non-zero on any failure (CI-friendly).
 *
 * If we outgrow it, swap in real Unity later — same RUN_TEST / UNITY_BEGIN /
 * UNITY_END / TEST_ASSERT_* surface, no test rewrites needed.
 */
#ifndef RDM_TEST_UNITY_H
#define RDM_TEST_UNITY_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int  unity_pass_count;
extern int  unity_fail_count;
extern int  unity_test_count;
extern const char *unity_current_test;

void unity_begin(void);
int  unity_end(void);
void unity_run_test(const char *name, void (*fn)(void));
void unity_fail(const char *file, int line, const char *msg);

#define UNITY_BEGIN()             unity_begin()
#define UNITY_END()               unity_end()
#define RUN_TEST(fn)              unity_run_test(#fn, fn)

#define TEST_FAIL(msg)            do { unity_fail(__FILE__, __LINE__, (msg)); return; } while (0)

#define TEST_ASSERT_TRUE(c) \
	do { if (!(c)) TEST_FAIL("expected TRUE: " #c); } while (0)

#define TEST_ASSERT_FALSE(c) \
	do { if  ((c)) TEST_FAIL("expected FALSE: " #c); } while (0)

#define TEST_ASSERT_NULL(p) \
	do { if ((p) != NULL) TEST_FAIL("expected NULL: " #p); } while (0)

#define TEST_ASSERT_NOT_NULL(p) \
	do { if ((p) == NULL) TEST_FAIL("expected non-NULL: " #p); } while (0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
	do { long long _e = (long long)(expected), _a = (long long)(actual); \
	     if (_e != _a) { char _b[160]; snprintf(_b, sizeof(_b), \
	         "expected %lld got %lld for " #actual, _e, _a); \
	         TEST_FAIL(_b); } } while (0)

#define TEST_ASSERT_EQUAL_INT64(expected, actual) TEST_ASSERT_EQUAL_INT(expected, actual)
#define TEST_ASSERT_EQUAL_UINT(expected, actual)  TEST_ASSERT_EQUAL_INT(expected, actual)

#define TEST_ASSERT_EQUAL_HEX(expected, actual) \
	do { unsigned long long _e = (unsigned long long)(expected), \
	                       _a = (unsigned long long)(actual); \
	     if (_e != _a) { char _b[160]; snprintf(_b, sizeof(_b), \
	         "expected 0x%llx got 0x%llx for " #actual, _e, _a); \
	         TEST_FAIL(_b); } } while (0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
	do { const char *_e = (expected), *_a = (actual); \
	     if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
	         char _b[256]; snprintf(_b, sizeof(_b), \
	             "expected \"%s\" got \"%s\" for " #actual, \
	             _e ? _e : "(null)", _a ? _a : "(null)"); \
	         TEST_FAIL(_b); } } while (0)

#define TEST_ASSERT_EQUAL_FLOAT(expected, actual) \
	do { double _e = (double)(expected), _a = (double)(actual); \
	     if (fabs(_e - _a) > 1e-5) { char _b[160]; snprintf(_b, sizeof(_b), \
	         "expected %.6f got %.6f for " #actual, _e, _a); \
	         TEST_FAIL(_b); } } while (0)

#endif /* RDM_TEST_UNITY_H */
