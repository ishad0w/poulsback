# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Check integration patch framing without requiring external Linux sources."""

from pathlib import Path
import re
import unittest


PATCH = Path(__file__).resolve().parents[2] / "kernel/integration/linux.patch"
HEADER = re.compile(r"@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@")


class KernelPatch(unittest.TestCase):
    def test_every_hunk_accounts_for_removed_and_added_source(self):
        lines = PATCH.read_text().splitlines()
        hunks = 0
        for index, line in enumerate(lines):
            match = HEADER.match(line)
            if not match:
                continue
            hunks += 1
            old = new = 0
            for record in lines[index + 1 :]:
                if record.startswith(("@@ ", "diff ", "--- ", "+++ ")) or not record:
                    break
                if record.startswith(" "):
                    old += 1
                    new += 1
                elif record.startswith("-"):
                    old += 1
                elif record.startswith("+"):
                    new += 1
                else:
                    self.assertEqual(record, r"\ No newline at end of file")
            with self.subTest(line=index + 1, header=line):
                self.assertEqual(
                    (old, new), tuple(int(n) if n is not None else 1 for n in match.groups())
                )
        self.assertGreater(hunks, 0)


if __name__ == "__main__":
    unittest.main()
