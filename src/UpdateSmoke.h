#pragma once

class QCoreApplication;

namespace fc {

// Runs one complete online update non-interactively -- check, download, verify
// SHA-256, install, relaunch -- narrating each stage on stdout and returning a
// process exit code (0 = a release was installed, 2 = already up to date,
// 1 = anything went wrong).
//
// It exists for the same reason MainWindow::runPackageSmoke does: the thing
// worth verifying before a release is the real path with a real server, and the
// real path ends by replacing this executable and starting the replacement,
// which no unit test can be allowed to do to a test runner. Not exposed through
// the UI. Point it at a server with:
//
//   set FILECOMMANDER_UPDATE_MANIFEST_URL=http://127.0.0.1:8765/version.json
//   FileCommander.exe --update-smoke
//
// See tools/mock-update-server.py for the other end.
int runUpdateSmoke(QCoreApplication &app);

} // namespace fc
