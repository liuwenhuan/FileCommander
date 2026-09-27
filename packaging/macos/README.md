# macOS Intel build

The supported package workflow currently produces one Intel `x86_64` app. It
uses Qt 5 and the archive/network libraries from the pinned vcpkg prefix, plus
Homebrew's `mpv` and Samba libraries.

Install the macOS-only dependencies first:

```bash
brew install mpv samba pkg-config
```

Prepare the Intel vcpkg prefix and build the application:

```bash
packaging/macos/bootstrap-deps.sh --arch x86_64
packaging/macos/build-x86_64.sh
```

The build script automatically:

- bundles the native `FileCommander.icns` app icon into the macOS bundle;
- builds Poppler's Qt5 bindings into `.deps/macos/poppler-qt5-x64`;
- enables PDF preview and libmpv video/audio preview;
- builds the pinned `office-oxide` revision and bundles it beside the app;
- builds the SMB provider and its isolated `FileCommander-smb-helper`;
- deploys Qt, Poppler, mpv, Samba/libsmbclient, and their transitive dylibs;
- verifies that every app binary and bundled dylib is Intel-only and has no
  Homebrew or build-cache absolute runtime dependency.

The app is written to:

```text
build/macos-x86_64-release/FileCommander.app
```

The icon is stored at:

```text
build/macos-x86_64-release/FileCommander.app/Contents/Resources/FileCommander.icns
```

The build also checks that `Contents/Info.plist` references this icon. If the
Finder or Dock still shows an old icon after rebuilding, restart Finder or
refresh LaunchServices before diagnosing the bundle.

The default deployment target is macOS 12.0. Set
`FILECOMMANDER_ADHOC_SIGN=0` to skip the local ad-hoc signature. Developer ID
signing, hardened runtime, and notarization remain release-environment steps.

Useful overrides:

```bash
FILECOMMANDER_DEPS_ROOT=/Volumes/build-cache/filecommander \
FILECOMMANDER_BUILD_JOBS=4 \
packaging/macos/build-x86_64.sh

FILECOMMANDER_POPPLER_VERSION=26.04.0 \
packaging/macos/build-poppler-qt5.sh
```
