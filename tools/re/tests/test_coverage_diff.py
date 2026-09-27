"""Unit tests for coverage_diff.py (no game data needed)."""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import coverage_diff  # noqa: E402


def write(data):
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(data, f)
    f.close()
    return f.name


def entry(addr, overlay="", symbol="", calls=0, first=0):
    return {"addr": addr, "overlay": overlay, "symbol": symbol, "calls": calls, "first_frame": first}


class DiffTest(unittest.TestCase):
    def run_diff(self, a, b, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = coverage_diff.main([a, b, *args])
        self.assertEqual(rc, 0)
        return out.getvalue()

    def test_only_in_b_and_a(self):
        a = write({"version": 1, "entries": [entry("0x80010000", calls=100)]})
        b = write({"version": 1, "entries": [entry("0x80010000", calls=100), entry("0x80020000", calls=50)]})
        out = self.run_diff(a, b)
        self.assertIn("0x80020000", out.split("only in B")[1].split("only in A")[0])
        # Reverse: now only in A.
        out = self.run_diff(b, a)
        self.assertIn("0x80020000", out.split("only in A")[1])

    def test_changed_counts(self):
        a = write({"version": 1, "entries": [entry("0x80010000", calls=100), entry("0x80010004", calls=100)]})
        b = write({"version": 1, "entries": [entry("0x80010000", calls=500), entry("0x80010004", calls=110)]})
        out = self.run_diff(a, b)
        changed = out.split("changed counts")[1]
        self.assertIn("0x80010000", changed)  # 5x: significant
        self.assertNotIn("0x80010004", changed)  # 1.1x: noise

    def test_min_calls_filters_noise(self):
        a = write({"version": 1, "entries": [entry("0x80010000", calls=2)]})
        b = write({"version": 1, "entries": [entry("0x80010000", calls=8)]})
        out = self.run_diff(a, b)
        self.assertNotIn("0x80010000", out.split("changed counts")[1])
        out = self.run_diff(a, b, "--min-calls", "1")
        self.assertIn("0x80010000", out.split("changed counts")[1])

    def test_overlay_keys(self):
        a = write({"version": 1, "entries": [entry("0x801E2A6C", "KAWSEG", "o_KAWSEG_801E2A6C", 10)]})
        b = write({"version": 1, "entries": [entry("0x801E2A6C", "KAWSEG", "o_KAWSEG_801E2A6C", 40)]})
        out = self.run_diff(a, b)
        self.assertIn("KAWSEG::0x801E2A6C", out)


if __name__ == "__main__":
    unittest.main()
