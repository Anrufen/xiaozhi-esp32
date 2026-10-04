#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# 将 scripts 目录加入 sys.path
SCRIPTS_DIR = Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import scan_secrets


class TestSecretScan(unittest.TestCase):
    def test_scan_bytes_clean(self):
        sample = b"Hello world! This is a clean binary firmware content."
        patterns = ["secret_pass_123", "admin_password", "192.168.2.14"]
        matches = scan_secrets.scan_bytes(sample, patterns)
        self.assertEqual(matches, [])

    def test_scan_bytes_matched(self):
        sample = b"Hello world! This contains secret_pass_123 and more."
        patterns = ["secret_pass_123", "admin_password"]
        matches = scan_secrets.scan_bytes(sample, patterns)
        self.assertEqual(matches, ["secret_pass_123"])

    def test_load_patterns_file(self):
        with tempfile.NamedTemporaryFile("w+", delete=False, encoding="utf-8") as f:
            f.write("# This is a comment\n")
            f.write("  pattern_one  \n")
            f.write("\n")
            f.write("pattern_two\n")
            f.write("# Another comment\n")
            temp_path = f.name

        try:
            patterns = scan_secrets.load_patterns_from_file(temp_path)
            self.assertEqual(patterns, ["pattern_one", "pattern_two"])
        finally:
            if os.path.exists(temp_path):
                os.remove(temp_path)

    def test_scan_file_clean_and_matched(self):
        with tempfile.NamedTemporaryFile("wb", delete=False) as f:
            f.write(b"\x00\x01\x02\x03my_secret_token_abc\x04\x05")
            temp_path = f.name

        try:
            # 命中
            is_clean, matches = scan_secrets.scan_file(
                temp_path, ["my_secret_token_abc", "other_thing"]
            )
            self.assertFalse(is_clean)
            self.assertEqual(matches, ["my_secret_token_abc"])

            # 未命中
            is_clean, matches = scan_secrets.scan_file(
                temp_path, ["clean_string", "not_there"]
            )
            self.assertTrue(is_clean)
            self.assertEqual(matches, [])
        finally:
            if os.path.exists(temp_path):
                os.remove(temp_path)

    def test_cli_exit_codes(self):
        scanner_script = str(SCRIPTS_DIR / "scan_secrets.py")

        with tempfile.NamedTemporaryFile("wb", delete=False) as f:
            f.write(b"Firmware binary data containing test_forbidden_key")
            bin_path = f.name

        try:
            # 命中应返回非零退出码 1
            cmd_hit = [
                sys.executable,
                scanner_script,
                bin_path,
                "--pattern",
                "test_forbidden_key",
            ]
            res_hit = subprocess.run(cmd_hit, capture_output=True, text=True)
            self.assertEqual(res_hit.returncode, 1)
            self.assertIn("扫描失败", res_hit.stderr)

            # 未命中应返回 0
            cmd_pass = [
                sys.executable,
                scanner_script,
                bin_path,
                "--pattern",
                "non_existent_key",
            ]
            res_pass = subprocess.run(cmd_pass, capture_output=True, text=True)
            self.assertEqual(res_pass.returncode, 0)
            self.assertIn("扫描通过", res_pass.stdout)
        finally:
            if os.path.exists(bin_path):
                os.remove(bin_path)


if __name__ == "__main__":
    unittest.main()
