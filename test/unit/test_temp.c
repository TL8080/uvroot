/*
 * Unit tests for pick_temp_directory() (src/path/temp.c).
 *
 * Added with the Android/Termux packaging support: the packaged binary
 * must work on a system without "/tmp", so the temporary directory is
 * now chosen from UVROOT_TMP_DIR, then TMPDIR (Termux exports it), then
 * a compiled-in default, instead of unconditionally falling back to
 * P_tmpdir.  The selection itself is a pure function and is tested
 * here; the file-system part (does the compiled-in default exist?) is
 * covered by the on-device smoke test.
 */
#include <check.h>
#include <stdlib.h>

#include "path/temp.h"

START_TEST(test_pick_uvroot_tmp_dir_wins)
{
    ck_assert_str_eq(pick_temp_directory("/a", "/b", "/c"), "/a");
}
END_TEST

START_TEST(test_pick_tmpdir_when_uvroot_unset)
{
    ck_assert_str_eq(pick_temp_directory(NULL, "/b", "/c"), "/b");
}
END_TEST

START_TEST(test_pick_fallback_when_both_unset)
{
    ck_assert_str_eq(pick_temp_directory(NULL, NULL, "/c"), "/c");
}
END_TEST

START_TEST(test_pick_ignores_empty_uvroot_tmp_dir)
{
    /* "UVROOT_TMP_DIR=" must not select the current directory: an
     * empty value is as good as an unset one. */
    ck_assert_str_eq(pick_temp_directory("", "/b", "/c"), "/b");
}
END_TEST

START_TEST(test_pick_ignores_empty_tmpdir)
{
    ck_assert_str_eq(pick_temp_directory(NULL, "", "/c"), "/c");
}
END_TEST

static Suite *temp_suite(void)
{
    Suite *s = suite_create("temp");

    TCase *tc_pick = tcase_create("pick_temp_directory");
    tcase_add_test(tc_pick, test_pick_uvroot_tmp_dir_wins);
    tcase_add_test(tc_pick, test_pick_tmpdir_when_uvroot_unset);
    tcase_add_test(tc_pick, test_pick_fallback_when_both_unset);
    tcase_add_test(tc_pick, test_pick_ignores_empty_uvroot_tmp_dir);
    tcase_add_test(tc_pick, test_pick_ignores_empty_tmpdir);
    suite_add_tcase(s, tc_pick);

    return s;
}

int main(void)
{
    Suite *s = temp_suite();
    SRunner *sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    int number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
