#include "FolderAssociation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#ifndef Q_OS_WIN
#include <QStandardPaths>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr auto kStateGroup = "folderAssociation";
constexpr auto kDesktopFile = "FileCommander.desktop";

QString stateKey(const QString &suffix) {
    return QString::fromLatin1(kStateGroup) + QLatin1Char('/') + suffix;
}

#ifdef Q_OS_WIN
const QStringList &windowsClasses() {
    static const QStringList classes = {
        QStringLiteral("Directory"), QStringLiteral("Drive"), QStringLiteral("Folder")};
    return classes;
}

const QString &windowsOpenVerbName() {
    static const QString verb = QStringLiteral("open");
    return verb;
}

QString registryPath(const QString &className, const QString &tail = {}) {
    QString path = QStringLiteral("Software\\Classes\\%1\\shell").arg(className);
    if (!tail.isEmpty())
        path += QLatin1Char('\\') + tail;
    return path;
}

bool readDefaultValue(const QString &path, QString *value, bool *exists) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    DWORD type = 0;
    DWORD bytes = 0;
    const LONG first = RegGetValueW(HKEY_CURRENT_USER, native.c_str(), nullptr,
                                    RRF_RT_REG_SZ, &type, nullptr, &bytes);
    if (first == ERROR_FILE_NOT_FOUND) {
        *exists = false;
        value->clear();
        return true;
    }
    if (first != ERROR_SUCCESS)
        return false;
    std::wstring data(bytes / sizeof(wchar_t), L'\0');
    const LONG second = RegGetValueW(HKEY_CURRENT_USER, native.c_str(), nullptr,
                                     RRF_RT_REG_SZ, &type, data.data(), &bytes);
    if (second != ERROR_SUCCESS)
        return false;
    if (!data.empty() && data.back() == L'\0')
        data.pop_back();
    *exists = true;
    *value = QString::fromWCharArray(data.c_str(), static_cast<int>(data.size()));
    return true;
}

bool writeDefaultValue(const QString &path, const QString &value) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const std::wstring data = value.toStdWString();
    return RegSetKeyValueW(HKEY_CURRENT_USER, native.c_str(), nullptr, REG_SZ,
                           data.c_str(), static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)))
           == ERROR_SUCCESS;
}

bool removeDefaultValue(const QString &path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const LONG result = RegDeleteKeyValueW(HKEY_CURRENT_USER, native.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

bool removeKeyTree(const QString &path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const LONG result = RegDeleteTreeW(HKEY_CURRENT_USER, native.c_str());
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

bool enableWindows(QSettings &state, QString *error) {
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (executable.isEmpty()) {
        *error = QStringLiteral("The FileCommander executable path is unavailable.");
        return false;
    }
    const QString command = QStringLiteral("\"") + executable +
                            QStringLiteral("\" \"%1\"");
    for (const QString &className : windowsClasses()) {
        const QString shellPath = registryPath(className);
        const QString prefix = QStringLiteral("windows/%1/").arg(className);
        QString previous;
        bool hadPrevious = false;
        if (!state.contains(stateKey(prefix + QStringLiteral("hadDefault"))) &&
            !readDefaultValue(shellPath, &previous, &hadPrevious)) {
            *error = QStringLiteral("Cannot read the Windows folder association.");
            return false;
        }
        if (!state.contains(stateKey(prefix + QStringLiteral("hadDefault")))) {
            state.setValue(stateKey(prefix + QStringLiteral("hadDefault")), hadPrevious);
            state.setValue(stateKey(prefix + QStringLiteral("default")), previous);
        }

        const QString openCommandPath = registryPath(
            className, windowsOpenVerbName() + QStringLiteral("/command"));
        QString previousOpenCommand;
        bool hadPreviousOpenCommand = false;
        if (!state.contains(stateKey(prefix + QStringLiteral("hadOpenCommand"))) &&
            !readDefaultValue(openCommandPath, &previousOpenCommand, &hadPreviousOpenCommand)) {
            *error = QStringLiteral("Cannot read the Windows folder open command.");
            return false;
        }
        if (!state.contains(stateKey(prefix + QStringLiteral("hadOpenCommand")))) {
            state.setValue(stateKey(prefix + QStringLiteral("hadOpenCommand")),
                           hadPreviousOpenCommand);
            state.setValue(stateKey(prefix + QStringLiteral("openCommand")), previousOpenCommand);
        }

        if (!writeDefaultValue(openCommandPath, command) ||
            !writeDefaultValue(shellPath, windowsOpenVerbName())) {
            *error = QStringLiteral("Cannot register the Windows folder association.");
            return false;
        }
        state.setValue(stateKey(prefix + QStringLiteral("registeredCommand")), command);
        if (!removeKeyTree(registryPath(className, QStringLiteral("FileCommander")))) {
            *error = QStringLiteral("Cannot remove the obsolete Windows folder association.");
            return false;
        }
    }
    state.sync();
    return state.status() == QSettings::NoError;
}

bool disableWindows(QSettings &state, QString *error) {
    for (const QString &className : windowsClasses()) {
        const QString shellPath = registryPath(className);
        const QString prefix = QStringLiteral("windows/%1/").arg(className);
        QString current;
        bool hasCurrent = false;
        if (!readDefaultValue(shellPath, &current, &hasCurrent)) {
            *error = QStringLiteral("Cannot read the Windows folder association.");
            return false;
        }
        if (hasCurrent && (current == windowsOpenVerbName() ||
                           current == QLatin1String("FileCommander"))) {
            const bool hadPrevious = state.value(stateKey(prefix + QStringLiteral("hadDefault")), false)
                                         .toBool();
            const QString previous = state.value(stateKey(prefix + QStringLiteral("default"))).toString();
            const bool restored = hadPrevious ? writeDefaultValue(shellPath, previous)
                                              : removeDefaultValue(shellPath);
            if (!restored) {
                *error = QStringLiteral("Cannot restore the previous Windows folder association.");
                return false;
            }
        }
        const QString openCommandPath = registryPath(
            className, windowsOpenVerbName() + QStringLiteral("/command"));
        QString currentOpenCommand;
        bool hasCurrentOpenCommand = false;
        if (!readDefaultValue(openCommandPath, &currentOpenCommand, &hasCurrentOpenCommand)) {
            *error = QStringLiteral("Cannot read the Windows folder open command.");
            return false;
        }
        const QString registeredCommand =
            state.value(stateKey(prefix + QStringLiteral("registeredCommand"))).toString();
        if (hasCurrentOpenCommand && !registeredCommand.isEmpty() &&
            currentOpenCommand == registeredCommand) {
            const bool hadPreviousOpenCommand =
                state.value(stateKey(prefix + QStringLiteral("hadOpenCommand")), false).toBool();
            const QString previousOpenCommand =
                state.value(stateKey(prefix + QStringLiteral("openCommand"))).toString();
            const bool restored = hadPreviousOpenCommand
                                      ? writeDefaultValue(openCommandPath, previousOpenCommand)
                                      : removeDefaultValue(openCommandPath);
            if (!restored) {
                *error = QStringLiteral("Cannot restore the previous Windows folder open command.");
                return false;
            }
        }
        if (!removeKeyTree(registryPath(className, QStringLiteral("FileCommander")))) {
            *error = QStringLiteral("Cannot remove the Windows folder association.");
            return false;
        }
        state.remove(stateKey(prefix));
    }
    state.sync();
    return state.status() == QSettings::NoError;
}
#else
QString mimeAppsPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("mimeapps.list"));
}

bool runXdgMime(const QStringList &arguments, QString *error) {
    QProcess process;
    process.start(QStringLiteral("xdg-mime"), arguments);
    if (!process.waitForStarted(3000) || !process.waitForFinished(5000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QStringLiteral("xdg-mime could not update the folder association.");
        return false;
    }
    return true;
}

QString currentLinuxDefault(QString *error) {
    QProcess process;
    process.start(QStringLiteral("xdg-mime"),
                  {QStringLiteral("query"), QStringLiteral("default"),
                   QStringLiteral("inode/directory")});
    if (!process.waitForStarted(3000) || !process.waitForFinished(5000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QStringLiteral("xdg-mime could not read the folder association.");
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

bool enableLinux(QSettings &state, QString *error) {
    const QString oldDefault = currentLinuxDefault(error);
    if (!error->isEmpty())
        return false;
    if (!state.contains(stateKey(QStringLiteral("linux/hadDefault")))) {
        state.setValue(stateKey(QStringLiteral("linux/hadDefault")), !oldDefault.isEmpty());
        state.setValue(stateKey(QStringLiteral("linux/default")), oldDefault);
    }
    if (!runXdgMime({QStringLiteral("default"), QString::fromLatin1(kDesktopFile),
                     QStringLiteral("inode/directory")}, error))
        return false;
    state.sync();
    return state.status() == QSettings::NoError;
}

bool disableLinux(QSettings &state, QString *error) {
    const QString current = currentLinuxDefault(error);
    if (!error->isEmpty())
        return false;
    if (current == QLatin1String(kDesktopFile)) {
        const bool hadPrevious = state.value(stateKey(QStringLiteral("linux/hadDefault")), false)
                                     .toBool();
        if (hadPrevious) {
            if (!runXdgMime({QStringLiteral("default"),
                             state.value(stateKey(QStringLiteral("linux/default"))).toString(),
                             QStringLiteral("inode/directory")}, error))
                return false;
        } else {
            QSettings mimeApps(mimeAppsPath(), QSettings::IniFormat);
            mimeApps.beginGroup(QStringLiteral("Default Applications"));
            mimeApps.remove(QStringLiteral("inode/directory"));
            mimeApps.endGroup();
            mimeApps.sync();
            if (mimeApps.status() != QSettings::NoError) {
                *error = QStringLiteral("Cannot remove the Linux folder association.");
                return false;
            }
        }
    }
    state.remove(stateKey(QStringLiteral("linux")));
    state.sync();
    return state.status() == QSettings::NoError;
}
#endif
} // namespace

#ifdef Q_OS_WIN
QString FolderAssociation::windowsOpenVerb() {
    return windowsOpenVerbName();
}
#endif

QStringList FolderAssociation::folderArguments(const QStringList &arguments) {
    QStringList folders;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument.startsWith(QLatin1Char('-')))
            continue;
        const QFileInfo info(argument);
        if (!info.isDir())
            continue;
        const QString path = QDir::cleanPath(info.absoluteFilePath());
        if (!folders.contains(path))
            folders.append(path);
    }
    return folders;
}

bool FolderAssociation::setEnabled(bool enabled, const QString &stateFilePath, QString *error) {
    if (error)
        error->clear();
    if (stateFilePath.isEmpty()) {
        if (error)
            *error = QStringLiteral("The FileCommander settings path is unavailable.");
        return false;
    }
    QString localError;
    QSettings state(stateFilePath, QSettings::IniFormat);
#ifdef Q_OS_WIN
    const bool ok = enabled ? enableWindows(state, &localError) : disableWindows(state, &localError);
#else
    const bool ok = enabled ? enableLinux(state, &localError) : disableLinux(state, &localError);
#endif
    if (!ok && error)
        *error = localError;
    return ok;
}
