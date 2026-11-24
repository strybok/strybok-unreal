// iOS: receives Universal Links.
//
// iOS hands a Universal Link to the app delegate through
// application:continueUserActivity:restorationHandler: (not through openURL:, which is what
// FIOSCoreDelegates::OnOpenURL reports and which only covers custom URL schemes).
//
// Unreal's IOSAppDelegate implements that method only when the engine itself is compiled with
// WITH_IOS_UNIVERSAL_LINKS=1 (it then forwards to FCoreDelegates::OnActivatedByProtocol). That is
// off by default and cannot be switched on for an installed (launcher) engine. So this file works
// with both builds, using the Objective-C runtime:
//   * method missing  -> it is added to IOSAppDelegate;
//   * method present  -> it is wrapped, and the original implementation still runs.
//
// The hook is installed from +load, i.e. before main() and before UIApplication looks at its
// delegate, so the link that cold-starts the app is not missed even though the engine and this
// module start much later. Until then the URL waits in FStrybokPlatformLinks.
//
// Unreal compiles .cpp files as Objective-C++ on Apple platforms, like the engine's own
// IOSAppDelegate.cpp. Nothing here depends on ARC being on or off.

#include "StrybokPlatformLinks.h"

#if PLATFORM_IOS

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

namespace
{
	typedef void (^FStrybokRestorationHandler)(NSArray<id<UIUserActivityRestoring>>*);
	typedef BOOL (*FStrybokContinueActivityFunction)(id, SEL, UIApplication*, NSUserActivity*, FStrybokRestorationHandler);

	/** Implementation that was there before us (engine or another plugin), if any. */
	IMP GStrybokOriginalContinueActivity = nullptr;

	bool StrybokCaptureUserActivity(NSUserActivity* Activity)
	{
		if (Activity == nil || ![Activity.activityType isEqualToString:NSUserActivityTypeBrowsingWeb])
		{
			return false;
		}
		NSString* Url = Activity.webpageURL.absoluteString;
		if (Url == nil || Url.length == 0)
		{
			return false;
		}
		const char* Utf8 = [Url UTF8String];
		if (Utf8 == nullptr)
		{
			return false;
		}
		FStrybokPlatformLinks::Push(std::string(Utf8));
		return true;
	}
}

void StrybokIOSInstallUniversalLinkHook()
{
	static dispatch_once_t Once;
	dispatch_once(&Once, ^{
		Class DelegateClass = NSClassFromString(@"IOSAppDelegate");
		if (DelegateClass == Nil)
		{
			NSLog(@"Strybok: IOSAppDelegate class not found, Universal Links will not be received");
			return;
		}

		SEL Selector = @selector(application:continueUserActivity:restorationHandler:);
		Method Existing = class_getInstanceMethod(DelegateClass, Selector);
		if (Existing != nullptr)
		{
			GStrybokOriginalContinueActivity = method_getImplementation(Existing);
			IMP Wrapper = imp_implementationWithBlock(^BOOL(id Self, UIApplication* Application, NSUserActivity* Activity, FStrybokRestorationHandler RestorationHandler) {
				const bool bCaptured = StrybokCaptureUserActivity(Activity);
				BOOL bHandledByOriginal = NO;
				if (GStrybokOriginalContinueActivity != nullptr)
				{
					bHandledByOriginal = ((FStrybokContinueActivityFunction)GStrybokOriginalContinueActivity)(Self, Selector, Application, Activity, RestorationHandler);
				}
				return (bCaptured || bHandledByOriginal) ? YES : NO;
			});
			// Replaces the method on IOSAppDelegate itself, also when it was inherited.
			class_replaceMethod(DelegateClass, Selector, Wrapper, method_getTypeEncoding(Existing));
		}
		else
		{
			IMP Added = imp_implementationWithBlock(^BOOL(id Self, UIApplication* Application, NSUserActivity* Activity, FStrybokRestorationHandler RestorationHandler) {
				return StrybokCaptureUserActivity(Activity) ? YES : NO;
			});
			const struct objc_method_description Description =
				protocol_getMethodDescription(@protocol(UIApplicationDelegate), Selector, NO, YES);
			class_addMethod(DelegateClass, Selector, Added, Description.types != nullptr ? Description.types : "B@:@@@?");
		}
	});
}

/** Exists only for its +load, which runs when the executable is loaded. */
@interface StrybokUniversalLinkLoader : NSObject
@end

@implementation StrybokUniversalLinkLoader

+ (void)load
{
	StrybokIOSInstallUniversalLinkHook();
}

@end

#endif // PLATFORM_IOS
