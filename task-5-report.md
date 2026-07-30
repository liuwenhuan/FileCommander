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
