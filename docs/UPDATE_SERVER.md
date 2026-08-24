# Update announcement site

FileCommander only announces updates. It never downloads, verifies, installs, replaces, or restarts itself. The release page gives users package links and SHA-256 values for manual verification.

## Public URLs

The production site is HTTPS-only:

```text
https://fc.aigutta.com/version.json
https://fc.aigutta.com/update.html
https://fc.aigutta.com/FileCommander-X.Y.Z-windows-x64.zip
https://fc.aigutta.com/FileCommander_X.Y.Z_amd64.deb
https://fc.aigutta.com/FileCommander-X.Y.Z-1.x86_64.rpm
https://fc.aigutta.com/FileCommander-X.Y.Z-x86_64.AppImage
```

`/updata.html` is a permanent compatibility redirect to `/update.html`. Plain HTTP redirects to the same HTTPS URL and is never advertised in the manifest or page.

## Manifest

`version.json` is schema 2. The application reads `schema`, `version`, `date`, and `notes` only. Package metadata is for the static page and release automation.

```json
{
  "schema": 2,
  "version": "1.2.3",
  "date": "2026-08-24",
  "notes": "Release notes",
  "packages": {
    "windows": { "x86_64": { "filename": "...zip", "url": "https://fc.aigutta.com/...", "sha256": "...", "size": 1 } },
    "deb": { "x86_64": { "filename": "...deb", "url": "https://fc.aigutta.com/...", "sha256": "...", "size": 1 } },
    "rpm": { "x86_64": { "filename": "...rpm", "url": "https://fc.aigutta.com/...", "sha256": "...", "size": 1 } },
    "appimage": { "x86_64": { "filename": "...AppImage", "url": "https://fc.aigutta.com/...", "sha256": "...", "size": 1 } }
  }
}
```

Windows x86 is intentionally not advertised or published.

## Generate a site

After the four final package files exist, generate a staging site with:

```bash
python3 tools/generate-update-site.py \
  --version 1.2.3 --date 2026-08-24 --notes-file RELEASE_NOTES.txt \
  --output build/update-site \
  FileCommander-1.2.3-windows-x64.zip \
  FileCommander_1.2.3_amd64.deb \
  FileCommander-1.2.3-1.x86_64.rpm \
  FileCommander-1.2.3-x86_64.AppImage
```

The generator computes hashes and sizes from the supplied bytes, copies packages to the output root, and atomically writes `version.json`, `update.html`, and basename-only `SHA256SUMS.txt`. Release versions must be exactly three numeric components (`X.Y.Z`), with every component at most `2147483647`, matching the desktop parser's signed 32-bit comparison bound. It rejects missing, unexpected, duplicated, malformed, or version-mismatched package names. `version.json` is capped at 60 KiB so it remains below the desktop checker's 64 KiB network limit; release notes must be shortened when generation rejects an oversized manifest.

Run its tests with:

```bash
python3 -m unittest tests/test_generate_update_site.py
```

## Publish ordering

The running account service reads `FILECOMMANDER_UPDATE_ROOT` but must not write it. A restricted deployment identity stages the four packages, verifies hashes, atomically publishes immutable package files, writes `update.html` and `SHA256SUMS.txt`, then replaces `version.json` last. A manifest must never point to a package that is not already available.
