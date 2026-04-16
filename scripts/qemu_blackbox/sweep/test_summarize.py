#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
from summarize import count_verify_errors, classify_point, summarize_run


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


class TestClassifyPoint(unittest.TestCase):
    def test_all_zero_is_pass(self):
        self.assertEqual(classify_point([0, 0, 0]), "PASS")

    def test_one_nonzero_is_flaky(self):
        self.assertEqual(classify_point([0, 0, 3]), "FLAKY")

    def test_two_nonzero_is_suspect(self):
        self.assertEqual(classify_point([0, 5, 2]), "SUSPECT")

    def test_all_nonzero_is_fail(self):
        self.assertEqual(classify_point([1, 5, 2]), "FAIL")


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
            result = summarize_run(stem)
        self.assertEqual(result["err_stderr"], 2)
        self.assertEqual(result["err_json"], 2)
        self.assertEqual(result["json_error_code"], 84)

    def test_missing_files_return_zeros(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "missing"
            result = summarize_run(stem)
        self.assertEqual(result["err_stderr"], 0)
        self.assertEqual(result["err_json"], 0)
        self.assertEqual(result["json_error_code"], 0)

    def test_bad_json_returns_zeros(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            stem = Path(tmpdir) / "run"
            (stem.parent / "run.stderr").write_text("")
            (stem.parent / "run.json").write_text("not valid json{{{")
            result = summarize_run(stem)
        self.assertEqual(result["err_json"], 0)
        self.assertEqual(result["json_error_code"], 0)


if __name__ == "__main__":
    unittest.main()
