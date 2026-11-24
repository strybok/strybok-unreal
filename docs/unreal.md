# Unreal Engine integration

The `Strybok` plugin receives verified HTTPS links (iOS Universal Links, Android App Links), asks
the Strybok API which in-app route a link stands for, and hands your game each link exactly once.
It is the Unreal implementation of the [SDK contract](./sdk-contract.md).

> **Status: not compiled against Unreal Engine yet.**
>
> - **Tested:** the engine-independent core (URL validation, lookup URL, retry and back-off,
>   response validation, duplicate protection, queue and exactly-once delivery, event body,
>   creating links). It is
>   plain C++ and has its own test suite that runs without the engine
>   (`Tests/run_core_tests.sh` in the plugin directory).
> - **Not tested:** the Unreal module around it (subsystem, HTTP, timers, Project Settings, the
>   iOS app-delegate hook, the Android manifest and `GameActivity` changes). It was written for
>   UE 5.3+ without an engine installation: it has **never been compiled with Unreal Build Tool and
>   never run on a device**. Expect to fix compile errors on first use and please report them.
>   Known gaps are listed [at the end](#known-gaps).
>
> Deferred deep linking is **not supported**: a link tapped before the game is installed is not
> restored after installation.

## 1. Prerequisites

1. Create an application in Strybok with your iOS and Android identifiers.
2. Read its setup data: `GET /v1/apps/:appId/integration` returns the managed hostname
   (for example `my-app.strybok.link`).
3. Check that Strybok serves the association files for that hostname, as described in the
   [iOS](./ios.md#1-tell-strybok-about-the-app) and
   [Android](./android.md#1-tell-strybok-about-the-app) guides (step 1 of each). The native
   project changes of those guides are replaced by the steps below.
4. Unreal Engine 5.3 or newer, a C++ project (the plugin ships as source and must be compiled).

## 2. Add the plugin

Clone the plugin repository into your project as `Plugins/Strybok` (or add it there as a git
submodule, or copy the contents of a clone):

```sh
cd MyGame
git clone https://github.com/strybok/strybok-unreal Plugins/Strybok
```

The result:

```text
MyGame/
  MyGame.uproject
  Plugins/
    Strybok/
      Strybok.uplugin
      Source/Strybok/...
```

Regenerate project files and build. Enable the plugin under _Edit → Plugins → Strybok_ if it is
not enabled already, or in `MyGame.uproject`:

```json
"Plugins": [{ "Name": "Strybok", "Enabled": true }]
```

To use the C++ API from your game module, add the dependency in `MyGame.Build.cs`:

```csharp
PrivateDependencyModuleNames.Add("Strybok");
```

## 3. Project Settings

_Edit → Project Settings → Plugins → Strybok_, saved to `Config/DefaultGame.ini`:

```ini
[/Script/Strybok.StrybokSettings]
bAutoConfigure=True
ApiBaseUrl=https://api.strybok.com
LinkHost=my-app.strybok.link
```

| Setting                  | Default                   | Meaning                                                                                      |
| ------------------------ | ------------------------- | -------------------------------------------------------------------------------------------- |
| `bAutoConfigure`         | `True`                    | Start the SDK from these settings when the game instance starts.                             |
| `ApiBaseUrl`             | `https://api.strybok.com` | Base URL of the API. A path prefix is preserved.                                             |
| `LinkHost`               | empty                     | Your managed hostname. Allowed host **and** the host of the generated Android intent filter. |
| `AdditionalAllowedHosts` | empty                     | More accepted hostnames (custom domain). Their native setup is manual, see below.            |
| `TimeoutSeconds`         | `5`                       | Time allowed for one resolve attempt.                                                        |
| `MaxRetries`             | `2`                       | Extra attempts after a timeout, network error or 5xx.                                        |
| `RetryDelaySeconds`      | `0.4`                     | Pause before the first retry; doubles for each further retry.                                |
| `DedupeWindowSeconds`    | `2`                       | The identical URL received again within this window is dropped.                              |
| `bReportAppOpens`        | `True`                    | Send a best-effort `app_open` event after a link was resolved.                               |
| `ClientKey`              | empty                     | Publishable key `pk_…`, only for creating links to share (section 6).                        |

While `LinkHost` is empty the SDK stays idle (one log line, no error). To choose the values at
runtime instead, turn `bAutoConfigure` off and call `Configure` yourself (section 6).

## 4. iOS setup

### Associated Domains entitlement

iOS only opens the game for links of domains listed in the app's entitlements. Unreal has no
Project Settings field for this key, so you provide an entitlements file.

1. Create `Build/IOS/Resources/MyGame.entitlements` in your project:

   ```xml
   <?xml version="1.0" encoding="UTF-8"?>
   <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
   <plist version="1.0">
   <dict>
       <key>com.apple.developer.associated-domains</key>
       <array>
           <string>applinks:my-app.strybok.link</string>
       </array>
   </dict>
   </plist>
   ```

   No `https://`, no path, the exact hostname. This file **replaces** the entitlements Unreal would
   generate, so add every other entitlement your game needs (push notifications
   `aps-environment`, Game Center, Sign in with Apple, ...).

2. Point the Xcode project generator at it in `Config/DefaultEngine.ini` (`/Game/` means the
   project directory):

   ```ini
   [/Script/MacTargetPlatform.XcodeProjectSettings]
   PremadeIOSEntitlements=(FilePath="/Game/Build/IOS/Resources/MyGame.entitlements")
   ; Optional, only if Shipping needs a different file:
   ; ShippingSpecificIOSEntitlements=(FilePath="/Game/Build/IOS/Resources/MyGameShipping.entitlements")
   ```

   The same fields are in _Project Settings → Platforms → Xcode Projects_ in recent engine
   versions. This needs the modern Xcode workflow (`bUseModernXcode=True` in the same ini
   section). With the legacy iOS packaging pipeline there is no supported setting for this
   entitlement; see [known gaps](#known-gaps).

3. Enable **Associated Domains** for the App ID in the Apple Developer portal and regenerate the
   provisioning profile, otherwise signing fails or the entitlement is stripped.

4. Verify the packaged app:

   ```sh
   codesign -d --entitlements :- Payload/MyGame.app | grep -A3 associated-domains
   ```

Then follow [iOS: verify on a device](./ios.md#3-verify-on-a-device) (physical device, tap the
link in Notes, Apple CDN delay, `?mode=developer`).

### How the link reaches the plugin

Nothing to do; this is for debugging. iOS delivers Universal Links through
`application:continueUserActivity:restorationHandler:`. Unreal's `IOSAppDelegate` implements that
method only when the engine is built with `WITH_IOS_UNIVERSAL_LINKS=1` (off by default, not
changeable for an installed engine). `FIOSCoreDelegates::OnOpenURL` is not involved: it reports
`openURL:`, which is custom URL schemes only, and Strybok links are always `https`.

The plugin therefore installs the method itself with the Objective-C runtime, from a `+load`
method that runs before `main()`: if `IOSAppDelegate` lacks the method it is added, and if it has
one (engine built with the define, or another plugin) it is wrapped and the original still runs.
The URL waits in a small buffer until the subsystem is configured.

If your project already receives Universal Links through its own native code, you can skip the
hook's result entirely and call `HandleUrl` with the URL instead; duplicates within the
de-duplication window are dropped.

## 5. Android setup

With `LinkHost` set, packaging adds everything through `Source/Strybok/Strybok_UPL.xml`:

- the intent filter on `com.epicgames.unreal.GameActivity`:

  ```xml
  <intent-filter android:autoVerify="true">
      <action android:name="android.intent.action.VIEW" />
      <category android:name="android.intent.category.DEFAULT" />
      <category android:name="android.intent.category.BROWSABLE" />
      <data android:scheme="https" android:host="my-app.strybok.link" />
  </intent-filter>
  ```

- Java glue in `GameActivity` (`onCreate` for the launch intent, `onNewIntent` for later ones)
  that passes the URL to the plugin through JNI. Intents re-delivered from the recents list
  (`FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY`) are ignored.

Check the result after packaging in
`Intermediate/Android/<arch>/gradle/app/src/main/AndroidManifest.xml` (the exact path differs
between engine versions; search for `autoVerify`). The packaging log contains
`Strybok: adding App Links intent filter for https://<host>`.

`LinkHost` is read from the project's `Game` ini at packaging time, so it must be in
`Config/DefaultGame.ini`. For each entry of `AdditionalAllowedHosts` add the intent filter
yourself, for example in _Project Settings → Platforms → Android → Advanced APK Packaging →
Extra Settings for `<activity>` section_.

Then follow [Android: verify on a device](./android.md#3-verify-on-a-device)
(`adb shell pm get-app-links <package>`, re-verification, simulating a tap). Remember the
SHA-256 fingerprint of **every** signing certificate, including Play App Signing.

## 6. Use it

One subscription covers every case:

| Situation                                  | What happens                                                                                          |
| ------------------------------------------ | ----------------------------------------------------------------------------------------------------- |
| **Cold start** (game launched by the link) | The launch link is resolved at start-up, kept until something binds `OnLinkReceived`, delivered once. |
| **Background** (game resumed by the link)  | Delivered on `OnLinkReceived`.                                                                        |
| **Foreground**                             | Delivered on `OnLinkReceived`.                                                                        |

If the platform reports the same tap twice, the duplicate is dropped (`DedupeWindowSeconds`).
The same link tapped again later is a new delivery.

Bind from an object that lives as long as the game, normally your **Game Instance**: a link can
arrive on any level, and a listener in a level Blueprint dies with the level. Links that arrive
while nothing is bound are queued and delivered to the first binder (one frame after it binds).

### Blueprint

In your Game Instance Blueprint, on **Event Init**:

1. **Get Game Instance Subsystem** → class **Strybok Subsystem**.
2. Drag off it → **Bind Event to On Link Received** → create a custom event `HandleLink` with a
   `Link` input (Strybok Resolved Link).
3. In `HandleLink`: **Break Strybok Resolved Link**, then **Get Route Segments**
   (`/invite/abc123` → `["invite", "abc123"]`) and branch on the first segment:
   - `invite` with two segments → store segment 1 in a Game Instance variable
     (`PendingInviteCode`), then **Open Level (by Name)** `Lobby`; the lobby reads the variable.
   - `shop` → **Open Level** `Shop`, or show the shop widget if you are already in the menu.
   - anything else → ignore.
4. Optional: **Get Payload String** (`Link`, key `invitationId`) reads one payload field.
5. Optional: **Bind Event to On Link Error** to tell the player that a link could not be opened.

Never feed `Route Path` or payload text into **Open Level**, **Execute Console Command** or asset
loading directly; always go through a branch on values your game knows.

### C++

```cpp
// MyGameInstance.h
UCLASS()
class UMyGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;

	/** Read and cleared by the lobby once it has loaded. */
	UPROPERTY(BlueprintReadWrite)
	FString PendingInviteCode;

private:
	UFUNCTION()
	void HandleLink(const FStrybokResolvedLink& Link);

	UFUNCTION()
	void HandleLinkError(const FStrybokError& Error);
};
```

```cpp
// MyGameInstance.cpp
#include "MyGameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "StrybokSubsystem.h"

void UMyGameInstance::Init()
{
	Super::Init(); // creates the subsystems

	if (UStrybokSubsystem* Strybok = GetSubsystem<UStrybokSubsystem>())
	{
		Strybok->OnLinkReceived.AddDynamic(this, &UMyGameInstance::HandleLink);
		Strybok->OnLinkError.AddDynamic(this, &UMyGameInstance::HandleLinkError);
	}
}

void UMyGameInstance::HandleLink(const FStrybokResolvedLink& Link)
{
	// Map the route onto things this game knows. Never open "whatever the link says".
	const TArray<FString> Segments = Link.GetRouteSegments();

	if (Segments.Num() == 2 && Segments[0] == TEXT("invite"))
	{
		PendingInviteCode = Segments[1];
		UGameplayStatics::OpenLevel(this, TEXT("Lobby"));
	}
	else if (Segments.Num() == 1 && Segments[0] == TEXT("shop"))
	{
		UGameplayStatics::OpenLevel(this, TEXT("Shop"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("Ignoring unknown link route %s"), *Link.RoutePath);
	}

	FString InvitationId;
	if (Link.GetPayloadString(TEXT("invitationId"), InvitationId))
	{
		// ...
	}
}

void UMyGameInstance::HandleLinkError(const FStrybokError& Error)
{
	if (Error.Kind == EStrybokErrorKind::LinkNotFound)
	{
		// e.g. show "This invitation is no longer valid."
	}
}
```

Lambdas work through the native delegates:

```cpp
Strybok->OnLinkReceivedNative().AddLambda([](const FStrybokResolvedLink& Link) { /* ... */ });
```

Configuring at runtime instead of through Project Settings (`bAutoConfigure=False`):

```cpp
FStrybokConfig Config;
Config.ApiBaseUrl = TEXT("https://api.strybok.com");
Config.AllowedHosts = { TEXT("my-app.strybok.link") };
Strybok->Configure(Config); // false + a log line when the values are unusable
```

### API

| Member                                                                           | Purpose                                                                                                |
| -------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `Configure(Config)`                                                              | Start with explicit options. Calling it again replaces the configuration.                              |
| `ConfigureFromSettings()`                                                        | Start with the Project Settings values (done automatically with `bAutoConfigure`).                     |
| `IsConfigured()` / `IsReady()`                                                   | Configured; and whether the launch link has been handled.                                              |
| `OnLinkReceived`                                                                 | Multicast delegate with `FStrybokResolvedLink` (`Uri`, `LinkId`, `AppId`, `RoutePath`, `PayloadJson`). |
| `OnLinkError`                                                                    | Multicast delegate with `FStrybokError` (`Kind`, `Message`, `Url`, `StatusCode`).                      |
| `GetInitialLink(OnComplete)`                                                     | The launch link, if not delivered through `OnLinkReceived` yet. Completes after it was resolved.       |
| `TryGetInitialLink(OutLink)`                                                     | Synchronous variant; `false` while not resolved yet.                                                   |
| `ResolveUrl(Url, OnComplete)`                                                    | Resolve a URL explicitly (QR scanner, tests). Does not fire `OnLinkReceived` or report an app open.    |
| `HandleUrl(Url)`                                                                 | Manual entry point: same flow as a link from the OS. Buffered when called before `Configure`.          |
| `IsStrybokUrl(Url)`                                                              | Whether a URL is one of this app's links.                                                              |
| `CreateLink(Request, OnComplete)` / `CreateLinkNative(Request, Lambda)`          | Create a short link to share, see [below](#create-a-link-to-share).                                    |
| `Shutdown()`                                                                     | Stop: nothing is delivered afterwards. Automatic when the game instance ends.                          |
| `OnLinkReceivedNative()` / `OnLinkErrorNative()`                                 | C++ delegates for lambdas; the same stream.                                                            |
| `FStrybokResolvedLink::GetPayloadString`, `GetPayloadObject`, `GetRouteSegments` | Helpers; `UStrybokBlueprintLibrary` exposes two of them to Blueprint.                                  |

Use **either** `OnLinkReceived` **or** `GetInitialLink` for start-up navigation. Both are safe
together (the link is handed out once), but a bound `OnLinkReceived` normally gets the launch link
first and `GetInitialLink` then completes with `bHasLink = false`.

All functions are game-thread only; all delegates fire on the game thread.

### Create a link to share

`CreateLink` asks the service for a short link to a screen of your game — the "Share" button. It
is new in 0.2.0. The rules behind it (checks, request, retries, errors) are in the tested core;
the subsystem function around them is, like the rest of the module, **not compiled yet**.

1. In the dashboard, open the application, switch on **Links from the app** and list the route
   patterns the game may create links for (for example `/product/*`). Routes outside the list are
   refused.
2. Put the application's **publishable client key** (`pk_…`) into _Project Settings → Plugins →
   Strybok → Client Key_ (`ClientKey=pk_…` in `DefaultGame.ini`), or into `FStrybokConfig`. The
   key is safe to ship inside the game: it can only create links for those patterns. **Never put
   a secret API key (`lb_…`) in a game**; it is not accepted here.

```cpp
FStrybokCreateLinkRequest Request;
Request.RoutePath = TEXT("/product/42");
Request.PayloadJson = TEXT("{\"productId\":42}"); // optional, must be a JSON object
Request.Title = TEXT("Red shoes");               // optional

Strybok->CreateLinkNative(Request, [](bool bSuccess, const FStrybokCreatedLink& Link, const FStrybokError& Error)
{
	if (bSuccess)
	{
		ShareText(Link.Url); // https://my-app.strybok.link/Ab3xK9
	}
	else if (Error.Kind == EStrybokErrorKind::RateLimited)
	{
		ShowTryAgainLater(Error.RetryAfterSeconds); // -1 when unknown
	}
});
```

In Blueprint call **Create Link** on the subsystem with a **Make Strybok Create Link Request**
node and an event for `OnComplete`. Only the route is required; fields left empty are not sent.
Asking again for the same content returns the same link.

It only runs when you call it and is independent of receiving links: it works before and without
`Configure` (then with the Project Settings; no `LinkHost` is needed), never fires
`OnLinkReceived` or `OnLinkError`, and reports its result only through `OnComplete`:

| `Kind`                           | Meaning                                                                                                             | Retry later?                   |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `Unauthorized`                   | No client key set (no network call), or the key was refused: wrong or replaced key, or the feature is switched off. | no                             |
| `RouteNotAllowed`                | `RoutePath` is not covered by the route patterns in the dashboard.                                                  | no                             |
| `Validation`                     | Bad route or payload (no network call), or rejected by the service; `Issues` lists what it found.                   | no                             |
| `RateLimited`                    | Too many requests, or the app's daily limit of new links is reached.                                                | yes, after `RetryAfterSeconds` |
| `Network` / `Timeout` / `Server` | No usable answer after the retries (`TimeoutSeconds`, `MaxRetries`, `RetryDelaySeconds` as for resolving).          | yes                            |
| `NotConfigured`                  | The API base URL is unusable.                                                                                       | no                             |

Errors never contain the client key or the payload. `Shutdown`, a new `Configure` and the end of
the game instance abandon requests in flight; their `OnComplete` is not called. Do not put
personal data in a route or payload: whoever receives the link can read them.

## 7. Errors

`ResolveUrl` completes with, and `OnLinkError` receives, an `FStrybokError`:

| `Kind`                | Meaning                                                                                                                                                              |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `UnsupportedLink`     | Not `https`, host not allowed, or not a single-slug path. Only from `ResolveUrl`, before any network call. Links from the OS that are not ours are ignored silently. |
| `LinkNotFound`        | Unknown, disabled or expired link (HTTP 404). Not retried.                                                                                                           |
| `Timeout` / `Network` | No answer after all retries (e.g. offline). The game keeps working; the link is simply not opened.                                                                   |
| `Server`              | Unexpected status (`StatusCode`) or malformed response.                                                                                                              |
| `NotConfigured`       | Unreal only: `ResolveUrl` was called before `Configure` or after `Shutdown`.                                                                                         |

Errors are also written to the `LogStrybok` category (without the URL, whose query may be
private).

## 8. Treat routes as untrusted input

`RoutePath` and `PayloadJson` are data from the network, chosen by whoever created the link.
Match the route against routes your game knows before navigating. Do not pass them to
`OpenLevel`, `ServerTravel`, console commands, `StaticLoadObject` or soft object paths.

## 9. Links that arrive before the player can act on them

A link can arrive during the splash screen, a login flow or a loading screen. Store what the link
asks for (as in `PendingInviteCode` above) and act when the game reaches a state where it makes
sense, instead of dropping the link or interrupting a load.

## 10. Analytics and privacy

After a link is resolved the SDK sends one best-effort `app_open` event
(`POST /v1/public/events`) containing a random event id, the link id, the platform
(`ios`/`android`, `unknown` in the editor) and the SDK version — no device identifiers. It never
delays delivery and all failures are ignored. Disable it with `bReportAppOpens=False`.

Only the link's host and slug are sent to `resolve`; query parameters and fragments stay on the
device.

## 11. Testing

**In the editor** (no OS involved): with the plugin configured, call `HandleUrl` from a Blueprint
or C++ with one of your real links, e.g. `https://my-app.strybok.link/invite777`. It takes the
same path as a link from the OS, including `OnLinkReceived`. `ResolveUrl` shows the result or
the error without navigating.

**The core without the engine** (any machine with clang or gcc):

```sh
# in the plugin directory (Plugins/Strybok, or a clone of strybok-unreal)
Tests/run_core_tests.sh            # builds into a temporary directory
Tests/run_core_tests.sh /tmp/out   # or into a directory of your choice
```

**On devices:** the scenarios of the [manual test plan](./manual-test-plan.md) apply unchanged
(cold start, background, foreground, offline, expired link).

## Troubleshooting

| Symptom                                                        | Check                                                                                                                                                                             |
| -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Plugin does not compile                                        | Expected at this stage, see the status note. Report the first error together with the engine version.                                                                             |
| Link opens the browser/store instead of the game               | Native association is not verified: entitlement / intent filter, see sections 4 and 5 and the [iOS](./ios.md) and [Android](./android.md) guides. The SDK is not involved yet.    |
| iOS: entitlement missing in the packaged app                   | `PremadeIOSEntitlements` path wrong or legacy Xcode workflow; Associated Domains not enabled for the App ID; stale provisioning profile.                                          |
| Android: no `autoVerify` filter in the manifest                | `LinkHost` empty or not in `Config/DefaultGame.ini`; look for the `Strybok:` line in the packaging log.                                                                           |
| Game opens but nothing happens                                 | `LogStrybok` output; `LinkHost` / allowed hosts; nothing bound to `OnLinkReceived` (links wait in the queue); API reachable from the device?                                      |
| `java.lang.UnsatisfiedLinkError: nativeStrybokOnUrl` in logcat | The plugin's native code is not in the build (plugin disabled for the target), or the activity class is not `com.epicgames.unreal.GameActivity`.                                  |
| Screen opens twice                                             | You navigate from both `OnLinkReceived` and your own URL handling, or call `HandleUrl` with a different URL string than the OS delivered (de-duplication compares the whole URL). |
| Works on tap in Notes/Messages, not from Instagram/Telegram    | In-app browsers often do not hand links to apps. Users get the fallback page with a manual button.                                                                                |

## Known gaps

- **Never compiled with Unreal Build Tool, never run on a device.** Everything outside
  `Source/Strybok/Private/StrybokCore` is unverified. Parts checked without the engine: the iOS
  hook logic (compiled against the iOS SDK and exercised on macOS with a stand-in app delegate),
  the JNI function and the Java glue (compiled against stub classes), the UPL file (well-formed
  XML, instructions checked against the UPL reference).
- **iOS, legacy packaging workflow:** no supported way to add the Associated Domains entitlement;
  only the modern Xcode workflow (`PremadeIOSEntitlements`) is documented.
- **iOS, UIScene:** the hook targets the app-delegate life cycle that Unreal uses. If a future
  engine version moves to scene delegates, Universal Links will arrive through
  `scene:continueUserActivity:` and the hook must be extended.
- **iOS, launch link timing:** the first URL received before the SDK is configured is the launch
  link. iOS reports it right after start-up, long before the game instance exists, so in practice
  it is always there; a URL that arrives later is still delivered through `OnLinkReceived`, but
  `GetInitialLink` does not report it.
- **Android:** only `LinkHost` gets a generated intent filter; additional hosts are manual.
  Assumes the stock `com.epicgames.unreal.GameActivity` (UE 5).
- **Listener detection** is polled once per frame (dynamic delegates have no "on bound" hook):
  queued links reach a new binder one frame later. A delegate entry whose object was destroyed
  without unbinding can still count as a listener until the engine cleans it up.
- **Engine HTTP timeouts** shorter than `TimeoutSeconds` surface as `Network` (retried) rather than
  `Timeout`.
- **No automation tests inside the engine** and no example project yet.
