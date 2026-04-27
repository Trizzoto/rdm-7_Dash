#include "unity.h"

int unity_pass_count = 0;
int unity_fail_count = 0;
int unity_test_count = 0;
const char *unity_current_test = NULL;

static int unity_test_failed_flag = 0;

void unity_begin(void) {
	unity_pass_count = 0;
	unity_fail_count = 0;
	unity_test_count = 0;
	printf("\n=== Tests ===\n");
}

int unity_end(void) {
	printf("\n=== %d tests, %d passed, %d failed ===\n",
		unity_test_count, unity_pass_count, unity_fail_count);
	return unity_fail_count == 0 ? 0 : 1;
}

void unity_run_test(const char *name, void (*fn)(void)) {
	unity_current_test = name;
	unity_test_failed_flag = 0;
	unity_test_count++;
	fn();
	if (unity_test_failed_flag) {
		unity_fail_count++;
	} else {
		unity_pass_count++;
		printf("  [PASS] %s\n", name);
	}
}

void unity_fail(const char *file, int line, const char *msg) {
	unity_test_failed_flag = 1;
	printf("  [FAIL] %s\n         %s:%d  %s\n",
		unity_current_test ? unity_current_test : "?",
		file, line, msg);
}
