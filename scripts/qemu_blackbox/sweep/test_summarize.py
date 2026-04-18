#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
from summarize import (
    count_verify_errors,
    count_io_u_errors,
    classify_point,
    summarize_run,
)


class TestCountVerifyErrors(unittest.TestCase):
    def test_zero_on_clean_output(self):
        self.assertEqual(count_verify_errors("no errors here\njust normal output"), 0)

    def test_one_match_bad_magic_header(self):
        self.assertEqual(count_verify_errors("verify: bad magic header at offset 0"), 1)

    def test_one_match_type_mismatch(self):
        self.assertEqual(count_verify_errors("fio: verify type mismatch"), 1)

    def test_three_matches_mixed_lines(self):
        text = (
            "verify: bad magic header\n"
            "some other line\n"
            "fio: verify type mismatch\n"
            "verify: bad magic header again\n"
        )
        self.assertEqual(count_verify_errors(text), 3)


class TestCountIoUErrors(unittest.TestCase):
    def test_zero_on_clean_output(self):
        self.assertEqual(count_io_u_errors("no errors here\n"), 0)

    def test_single_io_u_error(self):
        self.assertEqual(
            count_io_u_errors("fio: io_u error on file /dev/nvme0n1: I/O error"),
            1,
        )

    def test_mixed_with_verify(self):
        text = (
            "verify: bad magic header\n"
            "fio: io_u error on file /dev/nvme0n1\n"
            "fio: io_u error on file /dev/nvme0n1\n"
        )
        self.assertEqual(count_io_u_errors(text), 2)


class TestClassifyPoint(unittest.TestCase):
    def test_all_zero_is_pass(self):
        self.assertEqual(classify_point([0, 0, 0]), "PASS")

    def test_one_nonzero_is_flaky(self):
        self.assertEqual(classify_point([0, 0, 3]), "FLAKY")

    def test_two_nonzero_is_suspect(self):
        self.assertEqual(classify_point([0, 5, 2]), "SUSPECT")

    def test_all_nonzero_is_fail(self):
        self.assertEqual(classify_point([1, 5, 2]), "FAIL")

    def _clean_result(self):
        return {
            "err_stderr": 0,
            "io_u_errors": 0,
            "err_json": 0,
            "json_error_code": 0,
            "json_valid": True,
            "fio_exit": 0,
        }

    def test_dict_all_clean_is_pass(self):
        clean = self._clean_result()
        self.assertEqual(classify_point([clean, clean, clean]), "PASS")

    def test_dict_one_io_u_is_flaky(self):
        bad = dict(self._clean_result(), io_u_errors=3)
        clean = self._clean_result()
        self.assertEqual(classify_point([clean, bad, clean]), "FLAKY")

    def test_dict_invalid_json_is_fail_signal(self):
        broken = dict(self._clean_result(), json_valid=False)
        self.assertEqual(classify_point([broken, broken, broken]), "FAIL")

    def test_dict_non_zero_fio_exit_counts_as_fail(self):
        clean = self._clean_result()
        bad = dict(clean, fio_exit=1)
        self.assertEqual(classify_point([clean, clean, bad]), "FLAKY")

    def test_dict_json_error_code_counts_as_fail(self):
        clean = self._clean_result()
        bad = dict(clean, json_error_code=84)
        self.assertEqual(classify_point([clean, bad, bad]), "SUSPECT")


class TestSummarizeRun(unittest.TestCase):
    def test_reads_stderr_and_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "run"
            stderr_content = (
                "verify: bad magic header\n"
                "fio: verify type mismatch\n"
                "some normal line\n"
            )
            (stem.parent / "run.stderr").write_text(stderr_content)
            json_content = {"jobs": [{"error": 84, "total_err": 2}]}
            (stem.parent / "run.json").write_text(json.dumps(json_content))
            (stem.parent / "run.exit").write_text("0\n")
            result = summarize_run(stem)
        self.assertEqual(result["err_stderr"], 2)
        self.assertEqual(result["err_json"], 2)
        self.assertEqual(result["json_error_code"], 84)
        self.assertEqual(result["io_u_errors"], 0)
        self.assertTrue(result["json_valid"])
        self.assertEqual(result["fio_exit"], 0)

    def test_missing_files_return_zeros_but_flag_missing(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "missing"
            result = summarize_run(stem)
        self.assertEqual(result["err_stderr"], 0)
        self.assertEqual(result["err_json"], 0)
        self.assertEqual(result["json_error_code"], 0)
        self.assertFalse(result["json_valid"])
        self.assertIn("stderr", result["artifacts_missing"])
        self.assertIn("json", result["artifacts_missing"])

    def test_bad_json_reports_invalid_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "run"
            (stem.parent / "run.stderr").write_text("")
            (stem.parent / "run.json").write_text("not valid json{{{")
            result = summarize_run(stem)
        self.assertEqual(result["err_json"], 0)
        self.assertFalse(result["json_valid"])

    def test_empty_json_reports_invalid(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "run"
            (stem.parent / "run.stderr").write_text("")
            (stem.parent / "run.json").write_text("")
            result = summarize_run(stem)
        self.assertFalse(result["json_valid"])

    def test_reads_io_u_error_count(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "run"
            stderr_content = (
                "fio: io_u error on file /dev/nvme0n1: I/O error\n"
                "fio: io_u error on file /dev/nvme0n1: I/O error\n"
            )
            (stem.parent / "run.stderr").write_text(stderr_content)
            (stem.parent / "run.json").write_text(json.dumps({"jobs": [{"error": 5, "total_err": 2}]}))
            (stem.parent / "run.exit").write_text("1\n")
            result = summarize_run(stem)
        self.assertEqual(result["io_u_errors"], 2)
        self.assertEqual(result["fio_exit"], 1)
        self.assertEqual(result["json_error_code"], 5)


if __name__ == "__main__":
    unittest.main()
