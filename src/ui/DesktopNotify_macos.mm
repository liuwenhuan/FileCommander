#include "DesktopNotify.h"

#import <UserNotifications/UserNotifications.h>

#include <QDateTime>

namespace ttc {

void notify(const QString &title, const QString &body) {
    @autoreleasepool {
        UNUserNotificationCenter *center =
            [UNUserNotificationCenter currentNotificationCenter];
        if (!center)
            return;

        NSString *titleString =
            [NSString stringWithUTF8String:title.toUtf8().constData()];
        NSString *bodyString =
            [NSString stringWithUTF8String:body.toUtf8().constData()];
        if (!titleString)
            titleString = @"FileCommander";
        if (!bodyString)
            bodyString = @"";

        [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                                 UNAuthorizationOptionSound)
                              completionHandler:^(BOOL granted, NSError *) {
                                  if (!granted)
                                      return;
                                  UNMutableNotificationContent *content =
                                      [[UNMutableNotificationContent alloc] init];
                                  content.title = titleString;
                                  content.body = bodyString;
                                  content.sound = [UNNotificationSound defaultSound];

                                  NSString *identifier =
                                      [NSString stringWithFormat:@"filecommander-%lld",
                                                                 QDateTime::currentMSecsSinceEpoch()];
                                  UNNotificationRequest *request =
                                      [UNNotificationRequest requestWithIdentifier:identifier
                                                                             content:content
                                                                             trigger:nil];
                                  [center addNotificationRequest:request
                                               withCompletionHandler:nil];
#if !__has_feature(objc_arc)
                                  [content release];
#endif
                              }];
    }
}

} // namespace ttc
