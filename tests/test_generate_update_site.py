import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate-update-site.py"


class GenerateUpdateSiteTest(unittest.TestCase):
    VERSION = "1.2.3"
    ARTIFACTS = [
        "FileCommander-1.2.3-windows-x64-setup.exe",
        "FileCommander_1.2.3_amd64.deb",
        "FileCommander-1.2.3-1.x86_64.rpm",
        "FileCommander-1.2.3-x86_64.AppImage",
    ]

    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        artifacts = []
        for number, name in enumerate(self.ARTIFACTS):
            path = root / name
            path.write_bytes(f"artifact-{number}".encode())
            artifacts.append(path)
        notes = root / "notes.txt"
        notes.write_text("Release <notes> & details", encoding="utf-8")
        return temporary, root, notes, artifacts

    def generate(self, root, notes, artifacts, version=VERSION):
        output = root / "site"
        return subprocess.run(
            [sys.executable, str(GENERATOR), "--version", version, "--date", "2026-08-24",
             "--notes-file", str(notes), "--output", str(output), *map(str, artifacts)],
            text=True, capture_output=True, check=False), output

    def test_generates_manifest_page_checksums_and_packages(self):
        temporary, root, notes, artifacts = self.fixture()
        self.addCleanup(temporary.cleanup)
        result, output = self.generate(root, notes, artifacts)
        self.assertEqual(result.returncode, 0, result.stderr)

        manifest = json.loads((output / "version.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], 2)
        self.assertEqual(manifest["version"], self.VERSION)
        self.assertEqual(manifest["date"], "2026-08-24")
        self.assertEqual(manifest["notes"], "Release <notes> & details")
        self.assertEqual(set(manifest["packages"]), {"windows", "deb", "rpm", "appimage"})
        for package_type, architecture, name in [
            ("windows", "x86_64", self.ARTIFACTS[0]),
            ("deb", "x86_64", self.ARTIFACTS[1]),
            ("rpm", "x86_64", self.ARTIFACTS[2]),
            ("appimage", "x86_64", self.ARTIFACTS[3]),
        ]:
            entry = manifest["packages"][package_type][architecture]
            source = root / name
            self.assertEqual(entry["filename"], name)
            self.assertEqual(entry["url"], f"https://fc.aigutta.com/{name}")
            self.assertEqual(entry["size"], source.stat().st_size)
            self.assertEqual(entry["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertEqual((output / name).read_bytes(), source.read_bytes())

        page = (output / "update.html").read_text(encoding="utf-8")
        self.assertIn("Release &lt;notes&gt; &amp; details", page)
        self.assertNotIn("<notes>", page)
        checksums = (output / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines()
        self.assertEqual(len(checksums), 4)
        self.assertTrue(all("/" not in line.split("  ", 1)[1] for line in checksums))

    def test_rejects_wrong_or_missing_artifacts(self):
        temporary, root, notes, artifacts = self.fixture()
        self.addCleanup(temporary.cleanup)
        wrong = root / "wrong.zip"
        wrong.write_bytes(b"wrong")
        result, _ = self.generate(root, notes, [*artifacts[:3], wrong])
        self.assertEqual(result.returncode, 2)
        self.assertIn("artifacts must exactly match", result.stderr)

    def test_rejects_invalid_version_and_date(self):
        temporary, root, notes, artifacts = self.fixture()
        self.addCleanup(temporary.cleanup)
        result, _ = self.generate(root, notes, artifacts, version="v1.2.3")
        self.assertEqual(result.returncode, 2)
        self.assertIn("version must", result.stderr)
        result, _ = self.generate(root, notes, artifacts, version="1.2.2147483648")
        self.assertEqual(result.returncode, 2)
        self.assertIn("components must not exceed", result.stderr)

    def test_rejects_manifest_larger_than_desktop_limit(self):
        temporary, root, notes, artifacts = self.fixture()
        self.addCleanup(temporary.cleanup)
        notes.write_text("x" * (64 * 1024), encoding="utf-8")
        result, _ = self.generate(root, notes, artifacts)
        self.assertEqual(result.returncode, 2)
        self.assertIn("version.json exceeds", result.stderr)


if __name__ == "__main__":
    unittest.main()
