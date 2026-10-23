"""Release naming, collision handling and artifact integrity regression checks."""
from datetime import date
import json
from pathlib import Path
import tempfile
import unittest
import urllib.error

from common import checksum_sidecar, sha256
from publish import RECORD_NAMES, SDK_NAMES, reserve_tag, tag_candidate, validate_assets


class ReleaseTests(unittest.TestCase):
    def test_actual_version_and_same_day_suffix(self):
        day = date(2026, 10, 23)
        self.assertEqual(tag_candidate("1.0.0-alpha.1", day, 1), "1.0.0-alpha.1.26.10.23")
        self.assertEqual(tag_candidate("1.0.0-alpha.1", day, 2), "1.0.0-alpha.1.26.10.23.2")
        self.assertEqual(tag_candidate("2.1.0", day, 12), "2.1.0.26.10.23.12")

    def test_existing_and_concurrently_created_tags_are_preserved(self):
        existing = {"1.0.0-alpha.1.26.10.23"}
        posted = []

        def request(path, method="GET", data=None):
            if method == "GET":
                if path.rsplit("/", 1)[1] not in existing:
                    raise urllib.error.HTTPError(path, 404, "missing", {}, None)
                return {}
            tag = data["ref"].removeprefix("refs/tags/")
            posted.append(tag)
            existing.add(tag)
            if tag.endswith(".2"):
                raise urllib.error.HTTPError(path, 422, "race", {}, None)
            return {}

        result = reserve_tag("1.0.0-alpha.1", date(2026, 10, 23), "a" * 40, request)
        self.assertEqual(result, "1.0.0-alpha.1.26.10.23.3")
        self.assertEqual(len(existing), 3)
        self.assertEqual(len(posted), 2)

    def test_complete_set_and_source_identity_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            commit = "a" * 40
            for name, record in zip(SDK_NAMES, RECORD_NAMES):
                package = directory / name
                package.write_bytes(b"fixture sdk")
                checksum_sidecar(package)
                (directory / record).write_text(json.dumps({"source_commit": commit,
                    "archive_sha256": sha256(package), "archive_bytes": package.stat().st_size}))
            self.assertEqual(len(validate_assets(directory, commit)), 12)
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                validate_assets(directory, "b" * 40)
            (directory / SDK_NAMES[0]).write_bytes(b"corrupt sdk")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                validate_assets(directory, commit)

    def test_missing_platform_cannot_publish(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(FileNotFoundError):
                validate_assets(Path(temporary), "a" * 40)


if __name__ == "__main__":
    unittest.main()
