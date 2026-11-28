# Unreal Engine integration

The `Strybok` plugin receives verified HTTPS links (iOS Universal Links, Android App Links), asks
the Strybok API which in-app route a link stands for, and hands your game each link exactly once.
It is the Unreal implementation of the [SDK contract](./sdk-contract.md).

> **Status: not compiled against Unreal Engine yet.**
>
> - **Tested:** the engine-independent core (URL validation, lookup URL, retry and back-off,
>   response validation, duplicate protection, queue and exactly-once delivery, event body,
>   campaign values, creating links). It is
>   plain C++ and has its own test suite that runs without the engine
>   (`Tests/run_core_tests.sh` in the plugin directory).
> - **Not tested:** the Unreal module around it (subsystem, HTTP, timers, Project Settings, the
>   iOS app-delegate hook, the Android manifest and `GameActivity` changes). It was written for
>   UE 5.3+ without an engine installation: it has **never been compiled with Unreal Build Tool and
>   never run on a device**. Expect to fix compile errors on first use and please report them.
>   Known gaps are listed [at the end](#known-gaps).
>
> A link tapped before the game is installed is **not restored by default**. Since 0.3.0 the
> plugin has the optional, off-by-default [deferred links](#deferred-links-optional) of the SDK
> contract — but it ships **no store integration and never reads the clipboard**: on Android your
> game must supply the store's install referrer through its own native code, on iOS the player
> must paste the link. Only the logic in the core is tested; nothing of it has run on a device.

## 1. Prerequisites

1. Create an application in Strybok with your iOS and Android identifiers.
2. Read its setup data: `GET /v1/apps/:appId/integration` returns the managed hostname
   (for example `my-app.strybok.link`).
   If the application has an active [custom domain](./ios.md#custom-domain-optional), the same answer lists it in `allowedHosts`: declare it in the entitlement and the intent filter next to the managed hostname and pass both to `allowedHosts`.
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
| `bParameterLinks`        | `False`                   | Deliver links without an API call (section 6).                                               |
| `PayloadKeys`            | empty                     | `parameterLinks.payloadKeys` of the integration guide; used only with `bParameterLinks`.     |

Four more settings belong to [deferred links](#deferred-links-optional) and do nothing unless you
switch the first one on: `bDeferredLinks` (`False`), `bAllowProbabilisticMatching` (`False`),
`AndroidPackageName` and `IosBundleId` (empty).

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

| Member                                                                                                            | Purpose                                                                                                            |
| ----------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| `Configure(Config)`                                                                                               | Start with explicit options. Calling it again replaces the configuration.                                          |
| `ConfigureFromSettings()`                                                                                         | Start with the Project Settings values (done automatically with `bAutoConfigure`).                                 |
| `IsConfigured()` / `IsReady()`                                                                                    | Configured; and whether the launch link has been handled.                                                          |
| `OnLinkReceived`                                                                                                  | Multicast delegate with `FStrybokResolvedLink` (`Uri`, `LinkId`, `AppId`, `RoutePath`, `PayloadJson`, `Campaign`). |
| `OnLinkError`                                                                                                     | Multicast delegate with `FStrybokError` (`Kind`, `Message`, `Url`, `StatusCode`).                                  |
| `GetInitialLink(OnComplete)`                                                                                      | The launch link, if not delivered through `OnLinkReceived` yet. Completes after it was resolved.                   |
| `TryGetInitialLink(OutLink)`                                                                                      | Synchronous variant; `false` while not resolved yet.                                                               |
| `ResolveUrl(Url, OnComplete)`                                                                                     | Resolve a URL explicitly (QR scanner, tests). Does not fire `OnLinkReceived` or report an app open.                |
| `HandleUrl(Url)`                                                                                                  | Manual entry point: same flow as a link from the OS. Buffered when called before `Configure`.                      |
| `IsStrybokUrl(Url)`                                                                                               | Whether a URL is one of this app's links.                                                                          |
| `CreateLink(Request, OnComplete)` / `CreateLinkNative(Request, Lambda)`                                           | Create a short link to share, see [below](#create-a-link-to-share).                                                |
| `Shutdown()`                                                                                                      | Stop: nothing is delivered afterwards. Automatic when the game instance ends.                                      |
| `GetDeferredLinkStatus()`, `OnDeferredLinkStatusChanged`, `EnableDeferredLinks()`                                 | [Deferred links](#deferred-links-optional): status, and switching them on after start-up.                          |
| `RedeemPastedLink(Text, OnComplete)` / `RedeemPastedLinkNative`, `SkipPastedLink()`                               | Deferred links on iOS: hand over what the player pasted.                                                           |
| `AddInstallReferrerSource`, `SetDeviceFamilyProvider`, `SetLocaleProvider`, `SetPasteboardMayContainLinkProvider` | C++ only: what your game supplies for deferred links.                                                              |
| `OnLinkReceivedNative()` / `OnLinkErrorNative()`                                                                  | C++ delegates for lambdas; the same stream.                                                                        |
| `FStrybokResolvedLink::GetPayloadString`, `GetPayloadObject`, `GetRouteSegments`                                  | Helpers; `UStrybokBlueprintLibrary` exposes two of them to Blueprint, plus **Has Campaign**.                       |

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

### Links without an API call (optional)

New in 0.4.0, **off by default**. The rules are in the tested core; the settings and the
conversion around them are, like the rest of the module, **not compiled yet**, and nothing of it
has run on a device. An application can let any address of its hosts open a route of the game
without creating a link first: **path links** such as `https://my-app.strybok.link/product/42?id=7`
and **Firebase long links** `https://my-app.strybok.link/?link=…`. The owner switches them on in
the dashboard and chooses the allowed routes and the query keys that become the payload. Without
the option the plugin accepts only links with a single slug path, exactly as before, and drops
these URLs without a request.

To deliver them, copy `parameterLinks` of the integration guide — `GET /v1/apps/:appId/integration`,
shown on the dashboard's setup screen next to `allowedHosts` — into the Project Settings
(_Links without an API call_):

```ini
; Config/DefaultGame.ini
[/Script/Strybok.StrybokSettings]
bParameterLinks=True
+PayloadKeys=id
+PayloadKeys=lang
```

or into `FStrybokConfig` (`bParameterLinks`, `PayloadKeys`) before `Configure`, or into
`strybok::Config::parameterLinks` of the core. The guide gives `null` while both kinds are off;
then leave `bParameterLinks` off.

- **What is sent:** the path exactly as received, the first value of each configured key (1–256
  UTF-16 code units without control characters) and the `utm_*` values; for a long link the deep
  link rebuilt from its scheme, host and path with only its payload keys, plus `apn` and `ibi`.
  Nothing else of the URL, and never the fragment.
- **What you get:** `RoutePath` and `PayloadJson` of the link are what the server read — the route
  to navigate to, never the tapped path. Treat both as untrusted input (section 8). The `app_open`
  event carries the `LinkId` of the answer.
- **Errors:** the server decides which routes, keys and web domains are allowed. With the option,
  an address of your hosts that is no link (a typo, a bare host) reaches `OnLinkError` as
  `LinkNotFound`. An invalid list (more than 20 keys, a key twice, a key that is not 1–40
  characters of `A–Z a–z 0–9 _ . -`, or `utm_…`, `st`, `sd`, `si`, `strybok_click`) makes
  `Configure` fail with a `Strybok configuration rejected` error in the log, like an unusable
  `ApiBaseUrl`. Blank entries are ignored.
- **The keys are compiled into the game.** A key the owner adds later reaches players with your
  next release; until then installed builds send the keys they know. Never configure a key that
  carries personal data.
- **Deferred links** keep the single-slug rule (the answer's URL and a pasted link): the server
  restores only stored links.

The exact rules are in the [SDK contract](./sdk-contract.md#links-without-an-api-call).

### Deferred links (optional)

A person taps a link, does not have the game, installs it and opens it: with deferred links the
game can still receive that link. It is **off by default**, works very differently on the two
platforms, and has real costs on iOS. The rules the plugin follows are in the SDK contract
([Android](./sdk-contract.md#deferred-links-optional-android),
[iOS](./sdk-contract.md#deferred-links-on-ios-optional)); the server side, the dashboard
switches and what each option costs your users are in the [Android](./android.md) and
[iOS](./ios.md) guides ("Links tapped before the app is installed"). Read those first.

**What the plugin does and does not do.** The core implements the whole flow: counting attempts,
finding the click id, the redeem and match requests, "a normal link wins", delivery exactly once.
The plugin ships **no store integration of its own, no JNI for it and no pasteboard code**:

- **Android:** your game supplies the store's install referrer string through an _install
  referrer source_ — a C++ function you register, backed by your own Java and JNI code.
- **iOS:** your game supplies the text the player pasted, from a paste control of your own.
- Like the rest of the Unreal module, this glue has **not been compiled with Unreal Build Tool**.
  **Nothing has been tried on a device**, and the Huawei AppGallery and Samsung Galaxy Store
  flows are unverified even in the native Android SDK.

A deferred link arrives through `OnLinkReceived` like any other link (queued until something is
bound), with `bIsDeferred = true` and `Uri` set to the link's own URL. `GetInitialLink` never
returns it — it can arrive seconds after the first screen — and no `app_open` event is sent for
it. If a normal link opens the game before the deferred one is ready, the deferred one is dropped
(one launch, one navigation). Nothing about deferred links ever fires `OnLinkError`.

#### Settings

| Setting                       | Default | Meaning                                                                                                                                |
| ----------------------------- | ------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `bDeferredLinks`              | `False` | Run the check after start-up. With `False` nothing of this section happens: no source is asked, no request is made, nothing is stored. |
| `AndroidPackageName`          | empty   | Your Android application ID. Required on Android; the plugin does not read it from the platform.                                       |
| `IosBundleId`                 | empty   | Your iOS bundle identifier. Required on iOS.                                                                                           |
| `bAllowProbabilisticMatching` | `False` | iOS only. **Forbidden by Apple's rules**, see [below](#ios-option-b-probabilistic-matching-not-recommended).                           |

The same four fields exist in `FStrybokConfig`. Without the identifier of the platform the game
runs on — and in the editor and on every other platform — the status stays `Disabled` and
nothing is asked, sent or stored.

Reading the install referrer is access to information stored on the device, and whether you need
the player's consent first is yours to decide. To ask first, leave `bDeferredLinks` off and call
`EnableDeferredLinks()` after the consent screen: it runs the check once.

The plugin keeps two values per installation, `attempts` and `done`, in
`Saved/Strybok/DeferredLinks.txt` — never the referrer, the pasted text or a click id. The file
goes away with the game's data. If a device backup restores it into a fresh installation, the
check reports `NotFirstLaunch`: a link can be lost that way, never delivered twice.

#### Android: supply the install referrer

Register a source before the check starts asking. The dependable order is: leave `bDeferredLinks`
off in the settings, register, then enable.

```cpp
void UMyGameInstance::Init()
{
	Super::Init();
	UStrybokSubsystem* Strybok = GetSubsystem<UStrybokSubsystem>();

#if PLATFORM_ANDROID
	Strybok->AddInstallReferrerSource([](TFunction<void(const FStrybokInstallReferrerAnswer&)> Complete)
	{
		// YOUR code: start the store's asynchronous call and return at once. Call Complete
		// exactly once, from any thread, when the store has answered.
		MyGame::RequestPlayInstallReferrer(MoveTemp(Complete));
	});
#endif
	Strybok->EnableDeferredLinks(); // or later, after a consent screen
}
```

(With `bDeferredLinks=True` in the settings the check starts one frame after the launch link was
handled, so sources must then be registered in `Init` as above, right after `Super::Init()`. A
source added after the check began is not asked in that launch. With no source at all the check
ends as `Unavailable` and is **not repeated** on later launches.)

`MyGame::RequestPlayInstallReferrer` is yours to write; the plugin cannot do it for you without
adding a Google dependency to every build. What it has to do, for Google Play:

1. Add `com.android.installreferrer:installreferrer:2.2` (Google's Maven repository) to the
   Gradle build, for example with a UPL file of your game (`<buildGradleAdditions>`).
2. In Java: `InstallReferrerClient.newBuilder(context).build()`, then
   `startConnection(InstallReferrerStateListener)`.
3. In `onInstallReferrerSetupFinished(int responseCode)` map the result exactly like this, then
   always call `endConnection()`:

   | Result                                                                                                              | Answer                                                                                                          |
   | ------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
   | `OK` (0)                                                                                                            | `FStrybokInstallReferrerAnswer::MakeReferrer(…)` with `getInstallReferrer().getInstallReferrer()`, as it stands |
   | `SERVICE_UNAVAILABLE` (1), `SERVICE_DISCONNECTED` (-1), a `RemoteException`, `onInstallReferrerServiceDisconnected` | `MakeUnavailableForNow()` — a later launch tries again                                                          |
   | `FEATURE_NOT_SUPPORTED` (2), `DEVELOPER_ERROR` (3), the library is missing                                          | `MakeUnavailable()`                                                                                             |

4. Hand the answer to C++ through a JNI native method of your own and call `Complete` there.

Pass the referrer string through unchanged and do not log it. The plugin takes only its own
`strybok_click` value out of it, sends that value, your package name and the SDK version to
`POST /v1/public/deferred-links/redeem`, and forgets the rest. A source that has not answered
after 10 seconds counts as "unavailable for now". The check runs at most three times in the life
of an installation.

Other stores: register one source per store, in the order they should be asked (a store that did
not install the game simply has no click id, and the next source is asked). The contract
describes Samsung's `InstallReferrerClient`
(`store.galaxy.samsung.installreferrer:samsung_galaxystore_install_referrer:4.0.0`, same shape and
codes) and Huawei's content provider (`content://com.huawei.appmarket.commondata/item/5`, column 5) — both **unverified**: nobody has confirmed that those stores hand our value back. Huawei also
requires the user's consent before its provider is queried.

It only works when the owner switched "restore the link after install" on for the application,
the installation went through the store app from the page the link opened, within 24 hours of the
tap, and once.

#### iOS option A: the player pastes the link

Needs the owner setting `iosDeferredClipboard`. Visitors without the game then see a page with a
"Copy link and get the app" button before the App Store. In the game, the player pastes:

```cpp
// Called by YOUR paste control with the text the player pasted.
void UMyOnboarding::OnPasted(const FString& PastedText)
{
	UStrybokSubsystem* Strybok = GetGameInstance()->GetSubsystem<UStrybokSubsystem>();
	Strybok->RedeemPastedLinkNative(PastedText, [](EStrybokPastedLinkResult Result)
	{
		// Delivered: the link arrives through OnLinkReceived.
		// NotOurs: the clipboard held something else. Failed: let the player try again.
	});
}
```

Blueprint: **Redeem Pasted Link**.

- **The plugin never reads the pasteboard** and has no paste button. Unreal's UI cannot show
  Apple's `UIPasteControl` (the system paste button of iOS 16 and later, which pastes without the
  "Allow Paste" alert); you have to add it as a native view from your own Objective-C++ code, or
  use a paste action of your own, and hand the resulting string to `RedeemPastedLink`. Do not
  paste automatically: reading the pasteboard without the player's action shows a system alert
  and is exactly what this design avoids.
- Text that is not a link of your app with a valid code gives `NotOurs`: nothing is sent, stored
  or logged. Otherwise only the click id, your bundle id and the SDK version are sent.
- A pasted link is delivered even if a normal link was already delivered in this launch.
- Put the control on the first screen or in onboarding with a sentence that says what it does,
  and describe it in your App Review notes.

What it costs: **every** iOS visitor without the game gets one extra page and one extra tap
before the App Store; it works only if the person taps the copy button there **and** later taps
Paste in the game; it replaces what was on their clipboard; the code works once, for 24 hours.
Nobody publishes how many people complete this. Details: [iOS guide](./ios.md).

#### iOS option B: probabilistic matching (not recommended)

Needs the owner setting `iosDeferredMatching`, the per-link opt-in `allowProbabilisticMatch`
(links the game creates itself can never have it), **and** `bAllowProbabilisticMatching` in the
game. On the first launch the plugin then sends your bundle id, the device family and the first
preferred language to `POST /v1/public/deferred-links/match`, and the server compares the
connection's IP address with recent taps.

Read this before switching it on:

- **Apple forbids it.** It is fingerprinting in the sense of the App Store Review Guidelines and
  the Developer Program License Agreement, with or without the player's consent. The App Review
  and legal risk is the app owner's — yours — not Strybok's.
- **A match can be another person's link.** On a shared network (home or office Wi-Fi, a
  university, a mobile carrier) the first person to open the game gets the link. So
  **`bIsProbabilisticMatch` must be checked**: such a link must never by itself sign anyone in,
  accept an invitation, apply a personal discount or show personal content.
- **It often does not work at all:** with iCloud Private Relay or a VPN, or after a change of
  network, the browser and the game arrive from different addresses and nothing matches.
- **Privacy:** it makes Strybok process the player's IP address for your application. Your
  privacy policy and your App Store privacy labels must cover that, and consent may be required.
  This is not legal advice.
- **Accuracy is unknown** and cannot be measured: the counters show how often the server found
  nothing, not how often a match was wrong.

The plugin does not read the device family or the language itself. Supply both, or matching ends
as `Unavailable`:

```cpp
#if PLATFORM_IOS
Strybok->SetDeviceFamilyProvider([]() { return MyGame::IosDeviceFamily(); });   // "iphone", "ipad" or "ipod"
Strybok->SetLocaleProvider([]() { return MyGame::IosFirstPreferredLanguage(); }); // e.g. "en-GB"
#endif
```

In your Objective-C++ code: the family comes from `UIDevice.currentDevice.userInterfaceIdiom`
(`.phone` → `iphone`, or `ipod` when `UIDevice.currentDevice.model` starts with `iPod`; `.pad` →
`ipad`; anything else → return an empty string) and the language is
`NSLocale.preferredLanguages.firstObject`. Send nothing else; the server refuses any other field.

If option A is on as well, the pasted link should win over the guess. For that, optionally supply
`SetPasteboardMayContainLinkProvider`, implemented with
`UIPasteboard.generalPasteboard detectPatternsForPatterns:` and
`UIPasteboardDetectionPatternProbableWebURL` — the call Apple documents as not notifying the user;
it never reads the content. When it answers `true` the status becomes `AwaitingPaste` and matching
waits until `RedeemPastedLink` returns `NotOurs` or you call `SkipPastedLink()` (the player moved
on without pasting).

#### Status

`GetDeferredLinkStatus()` and the delegate `OnDeferredLinkStatusChanged` (C++:
`OnDeferredLinkStatusChangedNative()`) are for you and your QA, not for players — most people who
install a game never tapped a link, so finding nothing is the normal result.

| `EStrybokDeferredLinkStatus` | Meaning                                                                                                 |
| ---------------------------- | ------------------------------------------------------------------------------------------------------- |
| `Disabled`                   | Off, not configured, not Android or iOS, or the package name / bundle id is missing                     |
| `Pending`                    | Not started yet, or this launch could not finish (store or network not reachable); a later launch tries |
| `Checking`                   | Running                                                                                                 |
| `NotFirstLaunch`             | Finished in an earlier launch                                                                           |
| `Unavailable`                | Android: no source, or none can ever answer. iOS: nothing automatic is on (pasting still works)         |
| `NoReferrer`                 | The store's referrer has no click id of ours: an ordinary install                                       |
| `NotFound`                   | The server refused: unknown, too old, already used, or not this application's                           |
| `Superseded`                 | A normal link arrived first                                                                             |
| `Delivered`                  | Handed to `OnLinkReceived`                                                                              |
| `Failed`                     | Three launches could not finish, or the server's answer was unusable                                    |
| `AwaitingPaste`              | iOS matching waits for `RedeemPastedLink` or `SkipPastedLink`                                           |

`RedeemPastedLink` completes with `Delivered`, `NotOurs`, `NotFound`, `AlreadyHandled`,
`Disabled` or `Failed`. The status is written to `LogStrybok` at `Verbose`; the referrer, the
pasted text and the click id are never logged.

Calling `Configure` again starts a fresh check for the new configuration; if the earlier one had
not finished, that uses up another of the three attempts.

## 7. Errors

`ResolveUrl` completes with, and `OnLinkError` receives, an `FStrybokError`:

| `Kind`                | Meaning                                                                                                                                                                                                                             |
| --------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `UnsupportedLink`     | Not `https`, host not allowed, or not a single-slug path (with `bParameterLinks`: not a route path or long link either). Only from `ResolveUrl`, before any network call. Links from the OS that are not ours are ignored silently. |
| `LinkNotFound`        | Unknown, disabled or expired link (HTTP 404). Not retried.                                                                                                                                                                          |
| `Timeout` / `Network` | No answer after all retries (e.g. offline). The game keeps working; the link is simply not opened.                                                                                                                                  |
| `Server`              | Unexpected status (`StatusCode`) or malformed response.                                                                                                                                                                             |
| `NotConfigured`       | Unreal only: `ResolveUrl` was called before `Configure` or after `Shutdown`.                                                                                                                                                        |

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

Only the link's host and slug are sent to `resolve`, plus the five `utm_*` campaign parameters of
the tapped URL when they are valid (see below); every other query parameter and the fragment
stay on the device.

With [deferred links](#deferred-links-optional) switched on — they are off by default — the
plugin additionally sends, at most a few times per installation: on Android the click id found
in the install referrer your game supplied, the package name and the SDK version; on iOS the
click id of a pasted link, the bundle id and the SDK version. Only iOS probabilistic matching
(`bAllowProbabilisticMatching`, off by default and forbidden by Apple) goes further: it sends the
bundle id, device family and first preferred language, and the server uses the connection's IP
address to identify the device.

### Campaign values

A resolved link has `Campaign` (`FStrybokCampaign`): the `utm_source`, `utm_medium`,
`utm_campaign`, `utm_term` and `utm_content` of this open as the strings `Source`, `Medium`,
`Campaign`, `Term` and `Content`. A value that is not set is an **empty string**;
`Link.Campaign.IsEmpty()` (Blueprint: **Has Campaign** on the link) tells whether there is any.
In Blueprint, **Break Strybok Resolved Link** → **Break Strybok Campaign** gives the five strings.

```cpp
void UMyGameInstance::HandleLink(const FStrybokResolvedLink& Link)
{
	if (!Link.Campaign.Source.IsEmpty())
	{
		MyAnalytics::SetLabel(TEXT("link_source"), Link.Campaign.Source); // a label, nothing more
	}
	// ... navigate by Link.RoutePath as before
}
```

The query filter, the parsing of the answer and the event body are part of the tested core; the
`FStrybokCampaign` struct and its conversion belong to the Unreal module and are, like the rest of
it, not compiled against the engine yet.

- **Where the values come from.** The link's defaults — set in the dashboard (link form →
  "Campaign (optional)") or through the management API — with the `utm_*` parameters of the
  tapped URL on top, key by key. `https://my-app.strybok.link/invite777?utm_source=instagram`
  opens with source `instagram` whatever the default is; a link tapped without parameters gives
  the defaults. A URL can replace a default but cannot clear it.
- **Untrusted labels.** Whoever shares a link can write any `utm_*` value into its URL. Use the
  values as labels for your own analytics; never decide access, prices, rewards or navigation by
  them.
- **What is accepted.** Only these five lower-case names, and only values of 1–64 characters
  out of `A–Z a–z 0–9 . _ ~ -`. Anything else (`spring sale`, an e-mail address, non-Latin
  letters) is dropped silently, as if the parameter were absent — by the SDK and again by the
  server.
- **What is sent.** With the resolve request the SDK sends those five parameters when they are
  valid, and nothing else of the query and never the fragment. With the `app_open` event it
  reports the values exactly as resolve returned them. The dashboard's link analytics then
  shows a "Campaigns" breakdown by source, medium and campaign, with app opens and browser opens
  counted separately (term and content are not broken down).
- **Versions.** New in 0.3.0. Older versions send no query: their links carry no `campaign` and
  their app opens are counted under the link's defaults.

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
- **Deferred links:** the plugin has no store integration, no JNI for the install referrer and no
  paste control; the game supplies all three (see
  [Deferred links](#deferred-links-optional)). The flow itself is covered by the core's tests;
  the glue in the module (`FStrybokDeferredFileStore`, the provider wrappers, the subsystem
  functions) is as uncompiled as the rest, and nothing was tried on a device or against a real
  store. Shipped referrer sources may follow later.
- **No automation tests inside the engine** and no example project yet.
