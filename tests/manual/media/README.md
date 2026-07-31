# WMF Media Probe

This manual probe is the acceptance gate for enabling Windows Lite media preview.
It uses Qt 5 Multimedia with the Windows Media Foundation plugin and writes one
JSON result file per run.

Build and run:

```powershell
cmake -S . -B build/wmf-probe -G Ninja -DFILECOMMANDER_BUILD_WMF_PROBE=ON
cmake --build build/wmf-probe --target wmf_media_probe
tests/manual/media/generate-fixtures.ps1
build/wmf-probe/wmf_media_probe.exe --fixtures build/wmf-fixtures --json build/wmf-results.json --unc-root "\\localhost\FileCommanderMediaProbe"
```

Acceptance requires every mandatory case to pass on clean Windows 10 and Windows
11 systems without third-party codecs. If either run fails or is unavailable,
Windows Lite must remain on the `none` media backend.
