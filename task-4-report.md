# Task 4 Report: Bounded Directory-Tree Motion

## Status

Implemented bounded directory-tree expansion animation on branch
`codex/directory-tree-motion`, starting from `15ed8b4`.

## Implementation

- Uses `MotionPolicy::allowFor()` before user-triggered tree expansion.
- Enables animation only below 500 currently visible tree rows.
- Treats repeated keyboard expansions within 100 ms as rapid input and disables animation.
- Restores eligible animation after 250 ms without another expansion.
- Reduced motion expands immediately with animation disabled.
- Counts at most 500 rows per expansion attempt and changes no tree model, selection, scroll,
  navigation, async loading, network, QuickView, media, or packaging behavior.

## Tests

- `DirectoryTreeMotion.*`: 4 passed.
- `DirectoryTreeMotion.*:DirectoryTreeModelTest.*:TreeNavigationTest.*`: 25 passed.

## Concerns

- The task brief's exact existing-suite filter uses `DirectoryTreeModel.*` and
  `TreeNavigation.*`; the repository's actual suite names are `DirectoryTreeModelTest` and
  `TreeNavigationTest`. The expanded filter above exercised those suites.
- The worktree's shell did not initially export the local MSVC/Qt SDK paths. Verification used
  the installed Visual Studio environment and the repository's local SDK paths; this does not
  affect source changes.

## Fix Round 1

### Status

The initial `QTreeView::setAnimated()` implementation described above is superseded. Directory
tree disclosure feedback is now paint-only and never enables `QTreeView` geometry animation.

### Implementation

- Uses a local `DirectoryTreeView` paint overlay whose accent opacity follows the exact
  `MotionPolicy` fast duration and easing curve.
- Starts feedback only when a pointer disclosure click or keyboard action actually changes the
  expansion state. Programmatic expansion and asynchronous child delivery do not start feedback.
- Uses one 100 ms cadence tracker for pointer, keyboard, and mixed rapid input.
- Scans no more than 500 visible rows when deciding eligibility; 500 or more skips feedback.
- Observes effective reduced-motion policy changes with context-bound teardown. Enabling reduced
  motion during feedback stops it synchronously and clears the overlay.
- Leaves row geometry, selection, scrolling, expansion state, navigation, and async loading under
  the existing tree implementation.

### Tests

- TDD red run: all 11 replacement `DirectoryTreeMotion.*` tests failed against the original
  geometry-animation implementation.
- Focused green run: `DirectoryTreeMotion.*`: 11 passed.
- Tree-sync isolation: `FilePanelTreeSyncTest.*`: 2 passed.
- Full tree/navigation/motion verification: 80 tests across 11 suites passed, including directory
  tree model and feedback, tree roots, panel tree sync, navigation, `MotionPolicy`, tabs, network
  state, and operation progress.

### Concerns

- Live reduced-motion completion required a small context-bound observer in the integrated
  `MotionPolicy`; it emits only when the effective reduced state changes and introduces no new
  setting or persistence behavior.
- Visual feedback is deliberately skipped at the 500-visible-row boundary and during rapid input,
  while the underlying expand/collapse action remains immediate.
