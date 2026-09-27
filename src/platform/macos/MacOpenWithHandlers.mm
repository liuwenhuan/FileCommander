#include "OpenWithHandlers.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <QFileInfo>

namespace {

QString stringFromNSString(NSString *value) {
    return value ? QString::fromUtf8(value.UTF8String) : QString();
}

QString pathFromURL(NSURL *url) {
    return stringFromNSString(url.path);
}

NSString *bundleIdentifierForURL(NSURL *url) {
    NSBundle *bundle = [NSBundle bundleWithURL:url];
    return bundle.bundleIdentifier;
}

QString displayNameForURL(NSURL *url) {
    NSBundle *bundle = [NSBundle bundleWithURL:url];
    NSString *name = [bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
    if (!name)
        name = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
    if (!name)
        name = url.lastPathComponent.stringByDeletingPathExtension;
    return stringFromNSString(name);
}

NSArray<NSURL *> *applicationURLsForPath(NSString *path) {
    NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
    NSURL *fileURL = [NSURL fileURLWithPath:path];
    if ([NSFileManager.defaultManager fileExistsAtPath:path])
        return [workspace URLsForApplicationsToOpenURL:fileURL];

    NSString *extension = path.pathExtension;
    if (extension.length == 0)
        return @[];
    UTType *type = [UTType typeWithFilenameExtension:extension];
    if (!type)
        return @[];
    return [workspace URLsForApplicationsToOpenContentType:type];
}

} // namespace

namespace fc {

QVector<OpenWithHandler> openWithHandlers(const QString &filePath) {
    if (filePath.isEmpty())
        return {};

    @autoreleasepool {
        NSString *path = [NSString stringWithUTF8String:filePath.toUtf8().constData()];
        if (!path)
            return {};

        NSArray<NSURL *> *applications = applicationURLsForPath(path);
        if (!applications || applications.count == 0)
            return {};

        NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
        NSURL *fileURL = [NSURL fileURLWithPath:path];
        NSURL *defaultURL = nil;
        if ([NSFileManager.defaultManager fileExistsAtPath:path]) {
            defaultURL = [workspace URLForApplicationToOpenURL:fileURL];
        } else {
            NSString *extension = path.pathExtension;
            UTType *type = extension.length > 0
                               ? [UTType typeWithFilenameExtension:extension]
                               : nil;
            if (type)
                defaultURL = [workspace URLForApplicationToOpenContentType:type];
        }

        const QString defaultPath = pathFromURL(defaultURL);
        QVector<OpenWithHandler> handlers;
        for (NSURL *applicationURL in applications) {
            const QString applicationPath = pathFromURL(applicationURL);
            if (applicationPath.isEmpty())
                continue;

            OpenWithHandler handler;
            handler.displayName = displayNameForURL(applicationURL);
            if (handler.displayName.isEmpty())
                handler.displayName = QFileInfo(applicationPath).completeBaseName();
            handler.program = applicationPath;
            handler.iconPath = applicationPath;
            handler.token = stringFromNSString(bundleIdentifierForURL(applicationURL));
            handler.recommended =
                !defaultPath.isEmpty() &&
                QFileInfo(applicationPath).canonicalFilePath() ==
                    QFileInfo(defaultPath).canonicalFilePath();
            handlers.append(handler);
        }
        return tidyOpenWithHandlers(std::move(handlers));
    }
}

bool launchOpenWithHandler(const OpenWithHandler &handler, const QString &filePath) {
    if (handler.program.isEmpty() || filePath.isEmpty())
        return false;

    @autoreleasepool {
        NSString *applicationPath =
            [NSString stringWithUTF8String:handler.program.toUtf8().constData()];
        NSString *filePathString =
            [NSString stringWithUTF8String:filePath.toUtf8().constData()];
        if (!applicationPath || !filePathString)
            return false;

        NSURL *applicationURL = [NSURL fileURLWithPath:applicationPath];
        NSURL *fileURL = [NSURL fileURLWithPath:filePathString];
        NSError *error = nil;
        NSRunningApplication *application =
            [[NSWorkspace sharedWorkspace] openURLs:@[fileURL]
                               withApplicationAtURL:applicationURL
                                             options:NSWorkspaceLaunchDefault
                                       configuration:@{}
                                             error:&error];
        return application != nil && error == nil;
    }
}

} // namespace fc
