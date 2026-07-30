# Task 3 Report: Windows Package Allowlist

## Commit

- `build: reject invalid Windows package contents` on `codex/windows-package-allowlist`.

## Delivered

- The Windows verifier reads the Task 2 release manifest, resolves the matching
  profile, rejects files not recorded in provenance, and enforces every profile
  requirement and prohibition.
- All package EXE and DLL files must be valid PE files for the requested
  architecture. `FileCommander.exe` must use the Windows GUI subsystem.
- Release packages reject PDBs and debug Qt/MSVC DLL names with package-relative
  diagnostics.
- Optional previous-manifest comparison rejects a single-file increase over 20
  MiB or total growth over 15 percent. `-AcceptSizeChange` records the verified
  release manifest as the supplied baseline after all content checks succeed.

## Tests

- `powershell -ExecutionPolicy Bypass -File tests/packaging/test-windows-allowlist.ps1`
- `powershell -ExecutionPolicy Bypass -File tests/packaging/test-windows-gui-subsystem.ps1`
- `powershell -ExecutionPolicy Bypass -File tests/packaging/test-windows-manifest.ps1`
- `powershell -ExecutionPolicy Bypass -File tests/packaging/test-msvc-runtime.ps1`

All four pass in the isolated worktree.

## Self-Review

- Preserved the Task 1 MSVC/runtime and smoke logic; its existing regression
  suite remains green.
- Preserved Task 2 manifest generation and profile selection; its regression
  suite remains green.
- Added no build output, UI/media source, or probe-object changes.
- The focused fixtures cover unknown executables and objects, debug residue,
  wrong PE architecture, GUI subsystem, profile requirements/prohibitions, and
  both size thresholds.

## Concern

- This task verifies fixture-level packaging behavior. A full Windows package
  build and runtime smoke test still needs the machine-specific Qt, vcpkg,
  Poppler, mpv, Visual Studio, and Windows SDK inputs.
