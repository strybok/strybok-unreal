# Strybok (Unreal Engine plugin)

Receives iOS Universal Links / Android App Links, resolves them through the Strybok API to an
in-app route and payload, and delivers each link exactly once — on cold start, from the
background and in the foreground. Implements the [SDK contract](./docs/sdk-contract.md).

> **Status:** only the engine-independent core is compiled and tested. The Unreal module (UE 5.3+)
> was written without an engine installation: it has **not been compiled with Unreal Build Tool
> and not run on a device yet**. A link tapped before the game is installed is not restored by
> default; the optional [deferred links](#deferred-links-optional) need native code of your game.

```cpp
UStrybokSubsystem* Strybok = GetGameInstance()->GetSubsystem<UStrybokSubsystem>();
Strybok->OnLinkReceived.AddDynamic(this, &UMyGameInstance::HandleLink);

void UMyGameInstance::HandleLink(const FStrybokResolvedLink& Link)
{
	// Link.RoutePath == "/invite/abc123"; map it onto levels and screens the game knows.
}
```

`Link.Campaign` (`FStrybokCampaign`: `Source`, `Medium`, `Campaign`, `Term`, `Content`; an empty
string means "not set") holds the campaign values of the open (`utm_source` and the like).
Campaign values are the link's defaults (set in the dashboard) overridden by the `utm_*`
parameters of the tapped URL. They are labels chosen by whoever shared the link: use them for
your analytics, never to decide anything.

Create a link to share (needs the application's publishable client key `pk_…` in the Project
Settings, with "Links from the app" switched on and route patterns listed in the dashboard — never
a secret API key):

```cpp
FStrybokCreateLinkRequest Request;
Request.RoutePath = TEXT("/product/42");
Strybok->CreateLinkNative(Request, [](bool bSuccess, const FStrybokCreatedLink& Link, const FStrybokError& Error)
{
	// bSuccess: share Link.Url. Otherwise see Error.Kind.
});
```

Full guide (installation, Project Settings, iOS and Android setup, Blueprint, errors,
troubleshooting, known gaps): [`docs/unreal.md`](./docs/unreal.md).

## Links without an API call (optional)

Path links (`https://my-app.strybok.link/product/42?id=7`) and Firebase long links
(`https://my-app.strybok.link/?link=…`) open a route without a link made in advance, once the
owner switched them on. Copy `parameterLinks` of the integration guide
(`GET /v1/apps/:appId/integration`, shown on the dashboard's setup screen) into the Project
Settings:

```ini
; Config/DefaultGame.ini
[/Script/Strybok.StrybokSettings]
bParameterLinks=True
+PayloadKeys=id
+PayloadKeys=lang
```

(or `bParameterLinks` / `PayloadKeys` of `FStrybokConfig`). Only the path and the values of these
keys are sent, and `RoutePath` / `PayloadJson` of the link are what the server read, never the
tapped path. Off by default; without it nothing changes. New in 0.4.0: the rules are in the
tested core, the settings around them are not compiled with Unreal Build Tool yet. Details:
[guide](./docs/unreal.md#links-without-an-api-call-optional).

## Deferred links (optional)

Restoring a link that was tapped before the game was installed is **off by default**
(`bDeferredLinks`, or `EnableDeferredLinks()` after a consent screen). The core implements the
whole flow of the [SDK contract](./docs/sdk-contract.md#deferred-links-optional-android)
and is tested; the plugin ships **no store integration, no JNI for it and no pasteboard code**, so
your game supplies the platform part:

- **Android:** set `AndroidPackageName` and register an install referrer source in C++
  (`AddInstallReferrerSource`), backed by your own Java/JNI call of the store's
  `InstallReferrerClient`. It answers with the raw referrer string, "unavailable for now" or
  "unavailable". Google Play is specified from Google's documentation; the Huawei AppGallery and
  Samsung Galaxy Store flows are unverified even in the native Android SDK.
- **iOS, pasted link:** set `IosBundleId` and pass what the player pasted with a paste control of
  your own to `RedeemPastedLink`. The plugin never reads the pasteboard. It costs every iOS
  visitor without the game an extra page and tap, and works only if the person copies and later
  pastes.
- **iOS, probabilistic matching** (`bAllowProbabilisticMatching`, plus `SetDeviceFamilyProvider`
  and `SetLocaleProvider`): **forbidden by Apple's rules** — the App Review and legal risk is
  yours —, can deliver another person's link on a shared network, does not work with Private
  Relay or a VPN, has privacy-policy and privacy-label consequences, needs an opt-in per link,
  and its accuracy is unknown. Not recommended.

A deferred link arrives through `OnLinkReceived` with `bIsDeferred` set (never from
`GetInitialLink`, no `app_open` event). A link with `bIsProbabilisticMatch` is a guess: never
sign anyone in, accept an invitation or show personal content on the strength of it alone.
`GetDeferredLinkStatus()` tells what happened; nothing is reported through `OnLinkError`.

Nothing of this has been compiled with Unreal Build Tool or run on a device. Details, the
response-code mapping your Android code must implement and the costs of each iOS option:
[guide](./docs/unreal.md#deferred-links-optional),
[Android](./docs/android.md), [iOS](./docs/ios.md).

## Install

Clone the repository into your project as `Plugins/Strybok`:

```sh
git clone https://github.com/strybok/strybok-unreal Plugins/Strybok
```

Then set _Project Settings → Plugins → Strybok_:

```ini
; Config/DefaultGame.ini
[/Script/Strybok.StrybokSettings]
ApiBaseUrl=https://api.strybok.com
LinkHost=my-app.strybok.link
```

## Layout

| Path                                              | What                                                                                                             |
| ------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| `Strybok.uplugin`                                 | Plugin descriptor (one runtime module, `Strybok`).                                                               |
| `Source/Strybok/Private/StrybokCore/`             | The contract in portable C++17: standard library only, no Unreal headers, no exceptions, no RTTI.                |
| `Source/Strybok/Public/StrybokSubsystem.h`        | `UStrybokSubsystem` (game instance subsystem): the Blueprint and C++ API.                                        |
| `Source/Strybok/Public/StrybokTypes.h`            | `FStrybokResolvedLink`, `FStrybokCampaign`, `FStrybokCreatedLink`, `FStrybokError`, `FStrybokConfig`, delegates. |
| `Source/Strybok/Public/StrybokSettings.h`         | `UStrybokSettings` (Project Settings page).                                                                      |
| `Source/Strybok/Public/StrybokBlueprintLibrary.h` | Blueprint helpers: payload field, route segments, "has campaign".                                                |
| `Source/Strybok/Private/StrybokRuntime.*`         | Core ↔ engine: `FHttpModule` transport, `FTSTicker` timers, clock, string conversion.                            |
| `Source/Strybok/Private/IOS/`                     | Universal Links: adds/wraps `application:continueUserActivity:restorationHandler:`.                              |
| `Source/Strybok/Private/Android/`                 | JNI entry point for the intent URL.                                                                              |
| `Source/Strybok/Strybok_UPL.xml`                  | Android: `autoVerify` intent filter and `GameActivity` glue.                                                     |
| `Tests/`                                          | Standalone tests of the core.                                                                                    |

How it fits together: native code only drops URLs into a small thread-safe buffer
(`FStrybokPlatformLinks`). The subsystem drains it on the game thread once per frame and feeds the
core (`strybok::LinkEngine`), which owns every rule: which URLs are ours, what is sent to the API,
retries, response validation, duplicate protection, the queue for links that arrive before a
listener, campaign values, the `app_open` event, and the optional deferred-link check. Creating links is a separate class of the core
(`strybok::LinkCreator`) with its own transport and timers, so it never depends on receiving them.

## Tests

The core has no Unreal dependency, so its tests run anywhere with clang or gcc:

```sh
Tests/run_core_tests.sh              # builds into a temporary directory
Tests/run_core_tests.sh /tmp/out     # or into a directory of your choice
```

The script builds the suite as C++17 and C++20 with `-fno-exceptions -fno-rtti -Wshadow -Werror`
(the way Unreal compiles modules) plus AddressSanitizer and UBSan, and runs both binaries. The
cases marked `[ref]` in `Tests/core_tests.cpp` are ports of the reference list, the Flutter SDK's
test suite (github.com/strybok/strybok-flutter).

There are no in-engine automation tests yet.

## Licence

Apache 2.0, see [LICENSE](./LICENSE).
