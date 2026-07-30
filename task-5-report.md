# Motion Feedback Task 5 Report

## Status

Implemented on `codex/quickview-static-fade`, starting from `a3508b1`.

QuickView now applies one 100 ms opacity reveal from 0.85 to 1.0 only to
approved static pages. Audio, video, native/OpenGL surfaces, encrypted controls,
and download progress remain immediate and interactive.

## Implementation

- Added `QuickView::revealStaticPage(QWidget *page)` with a strict static-page
  allow-list and explicit native/OpenGL surface exclusions.
- Kept a single owned opacity animation. A newer preview finishes and replaces
  the prior reveal, and the temporary graphics effect is removed at completion.
- Made reduced motion synchronous, including a live preference change while a
  reveal is active.
- Preserved accepted content while image, Markdown, PDF, Office, and archive
  work is pending. Only the current generation can install content and reveal it.
- Moved PDF loading off the GUI thread and retained the previous document until
  the accepted PDF is ready.
- Preserved the visible image viewport during a same-file reload that is waiting
  for an asynchronous rotation write, without changing existing loader
  synchronization behavior.
- Kept lazy page creation, the single media engine, and first-use media
  initialization intact.

## TDD Evidence

The new tests were written before the implementation.

1. The first build failed because `QuickView::revealStaticPage` did not exist.
2. The rapid image-generation test failed until pending image renders retained
   old content and only the winning generation triggered the reveal.
3. The dependency-enabled PDF readiness test failed while PDF loading was
   synchronous, then passed after generation-guarded asynchronous loading.
4. The existing same-path rotation reload test exposed a regression when the
   old image label remained populated. The viewport snapshot preserves the
   visible content while retaining the existing empty-label synchronization
   marker.

## Verification

- Debug `ui_tests` build: passed.
- Requested Debug filter
  `QuickViewMotion.*:QuickViewLazyPages.*:ImagePreviewLoader.*`: 32/32 passed.
- Full dependency Release `ui_tests` build with Poppler and mpv: passed.
- Release filter
  `QuickViewMotion.*:QuickViewLazyPages.*:ImagePreviewLoader.*:MediaEngineContract.*:MpvMediaEngine.*`:
  42 passed and 11 mpv fixture tests skipped because ffmpeg could not generate
  their local media fixtures. The real PDF readiness test ran and passed.
- `git diff --check`: passed.

## Concerns

- The complete 439-test UI binary was attempted with a 120-second timeout. It
  timed out late in the run and exposed unrelated environment/order-sensitive
  failures in text-encoding classification, Notepad persistence, and Qt
  standard-button localization. The scoped preview, image, lazy-page, PDF, and
  media verification above passed.
- The Poppler SDK uses the Release CRT, so the real PDF test was validated in the
  full Release test build rather than the dependency-enabled Debug build.
- The task has no deterministic external Office converter fixture. Office
  readiness uses the same existing generation guard, and its accepted result
  paths now reveal only after conversion completes.

## Fix Round 1/5

### Status

Addressed every finding in `task-5-review.md`. Static reveal effects now attach
to content-only child surfaces instead of whole QuickView pages. Page chrome,
toolbars, tabs, table widgets, headers, scrollbars, encrypted controls,
audio/video surfaces, native/OpenGL surfaces, and progress controls remain
effect-free, enabled, and immediately interactive.

### Implementation

- Added a page-to-content resolver for image, text, Markdown, PDF, archive,
  slides, Office grids, and info outcomes.
- Track the animated content target with `QPointer<QWidget>`. Replacement,
  reduced motion, theme refresh, and teardown restore and detach only the
  active target effect.
- Kept accepted-result routing, preview generations, cancellation, lazy page
  construction, `ImagePreviewLoader`, the single media engine, and first-use
  media initialization unchanged.
- Added a deterministic Office fixture executable that implements the existing
  converter commands. Tests still cross the real debounce, worker, subprocess,
  parser, generation, and accepted-result boundaries.

### TDD Evidence

The corrected boundary test was written first and failed because every
approved whole page had an opacity effect while each expected content viewport
had none. After moving the target, the boundary test and existing generation
tests passed. Route and pending-work tests were then added for Markdown,
archive, Office document/grid, slides, info, image render, and PDF paths.

### Verification

- Debug focused filter
  `QuickViewMotion.*:QuickViewLazyPages.*:ImagePreviewLoader.*`: 41/41 passed.
- Dependency-enabled Release focused filter
  `QuickViewMotion.*:QuickViewLazyPages.*:ImagePreviewLoader.*:MediaEngineContract.*:MpvMediaEngine.*`:
  52 passed, 11 skipped, 0 failed. All 16 QuickView motion tests, including the
  two Poppler PDF tests, passed.
- Both Debug and Release `ui_tests` targets built successfully.
- `git diff --check`: passed; only repository line-ending conversion warnings
  were reported.

### Concerns

- The 11 Release mpv integration tests were skipped because ffmpeg could not
  generate their local media fixtures in this environment. The six media
  contract tests and all lazy-page/media initialization tests passed.
- The Office fixture validates File Commander routing, process handling,
  parsing, generation checks, theme refresh, and teardown. It does not validate
  the external converter's fidelity on real Office documents.
- The previously observed unrelated full-suite environment/order-sensitive
  failures were not changed by this focused correction.
