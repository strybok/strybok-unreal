# Strybok (Unreal Engine plugin)

Receives iOS Universal Links / Android App Links, resolves them through the Strybok API to an
in-app route and payload, and delivers each link exactly once — on cold start, from the
background and in the foreground. Implements the [SDK contract](./docs/sdk-contract.md).

> **Status:** only the engine-independent core is compiled and tested. The Unreal module (UE 5.3+)
> was written without an engine installation: it has **not been compiled with Unreal Build Tool
> and not run on a device yet**. Deferred deep linking is not supported.

```cpp
UStrybokSubsystem* Strybok = GetGameInstance()->GetSubsystem<UStrybokSubsystem>();
Strybok->OnLinkReceived.AddDynamic(this, &UMyGameInstance::HandleLink);

void UMyGameInstance::HandleLink(const FStrybokResolvedLink& Link)
{
	// Link.RoutePath == "/invite/abc123"; map it onto levels and screens the game knows.
}
```

Full guide (installation, Project Settings, iOS and Android setup, Blueprint, errors,
troubleshooting, known gaps): [`docs/unreal.md`](./docs/unreal.md).

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

| Path                                              | What                                                                                              |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| `Strybok.uplugin`                                 | Plugin descriptor (one runtime module, `Strybok`).                                                |
| `Source/Strybok/Private/StrybokCore/`             | The contract in portable C++17: standard library only, no Unreal headers, no exceptions, no RTTI. |
| `Source/Strybok/Public/StrybokSubsystem.h`        | `UStrybokSubsystem` (game instance subsystem): the Blueprint and C++ API.                         |
| `Source/Strybok/Public/StrybokTypes.h`            | `FStrybokResolvedLink`, `FStrybokError`, `FStrybokConfig`, delegates.                             |
| `Source/Strybok/Public/StrybokSettings.h`         | `UStrybokSettings` (Project Settings page).                                                       |
| `Source/Strybok/Public/StrybokBlueprintLibrary.h` | Blueprint helpers: payload field, route segments.                                                 |
| `Source/Strybok/Private/StrybokRuntime.*`         | Core ↔ engine: `FHttpModule` transport, `FTSTicker` timers, clock, string conversion.             |
| `Source/Strybok/Private/IOS/`                     | Universal Links: adds/wraps `application:continueUserActivity:restorationHandler:`.               |
| `Source/Strybok/Private/Android/`                 | JNI entry point for the intent URL.                                                               |
| `Source/Strybok/Strybok_UPL.xml`                  | Android: `autoVerify` intent filter and `GameActivity` glue.                                      |
| `Tests/`                                          | Standalone tests of the core.                                                                     |

How it fits together: native code only drops URLs into a small thread-safe buffer
(`FStrybokPlatformLinks`). The subsystem drains it on the game thread once per frame and feeds the
core (`strybok::LinkEngine`), which owns every rule: which URLs are ours, what is sent to the API,
retries, response validation, duplicate protection, the queue for links that arrive before a
listener, and the `app_open` event.

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
