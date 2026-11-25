## 0.3.0

- Campaign values. `FStrybokResolvedLink::Campaign` (`FStrybokCampaign`, Blueprint-readable:
  `Source`, `Medium`, `Campaign`, `Term`, `Content`; an empty string means "not set";
  `IsEmpty()`) holds the link's default campaign values overridden by the `utm_*` parameters of
  the tapped URL, as the service returned them. Blueprint: **Break Strybok Campaign**, and
  **Has Campaign** in `UStrybokBlueprintLibrary`. The values are untrusted labels chosen by
  whoever shared the link.
- Core: the resolve request now carries the five parameters `utm_source`, `utm_medium`,
  `utm_campaign`, `utm_term` and `utm_content` of the tapped URL — exact lower-case names, the
  first occurrence of each, percent-decoded, and only values of 1–64 characters out of
  `A–Z a–z 0–9 . _ ~ -`, in a fixed order (`strybok::BuildCampaignQuery`, a small strict parser
  that takes any bytes: a malformed escape or invalid UTF-8 only drops that value). Every other
  query parameter and the fragment are still never sent, and a URL without valid campaign
  parameters produces exactly the request of earlier versions.
- Core: `strybok::Campaign`, `ResolvedLink::campaign`; `BuildAppOpenBody` takes the campaign and
  reports it exactly as resolve returned it, leaving it out when there is none. Covered by the
  standalone tests (C++17 and C++20, AddressSanitizer and UBSan).
- Deferred links (optional, **off by default**): restoring a link that was tapped before the game
  was installed, as the SDK contract specifies for SDKs without native store code. The plugin
  ships **no store integration, no JNI for it and no pasteboard code**; the game supplies the
  install referrer (Android) or the pasted text (iOS).
  - Settings and `FStrybokConfig`: `bDeferredLinks`, `bAllowProbabilisticMatching` (iOS;
    **forbidden by Apple's rules**, not recommended), `AndroidPackageName`, `IosBundleId`. Without
    the identifier of the running platform, and off Android and iOS, the status stays `Disabled`.
  - `UStrybokSubsystem`: `GetDeferredLinkStatus()`, `OnDeferredLinkStatusChanged`
    (`OnDeferredLinkStatusChangedNative()`), `EnableDeferredLinks()` (switch on after start-up,
    for example after a consent screen), `RedeemPastedLink` / `RedeemPastedLinkNative`,
    `SkipPastedLink()`; C++ only: `AddInstallReferrerSource`, `SetDeviceFamilyProvider`,
    `SetLocaleProvider`, `SetPasteboardMayContainLinkProvider`.
  - `EStrybokDeferredLinkStatus`, `EStrybokPastedLinkResult`, `FStrybokInstallReferrerAnswer`,
    `FStrybokInstallReferrerSource`.
  - `FStrybokResolvedLink` gains `bIsDeferred` and `bIsProbabilisticMatch` (both `false` for
    ordinary links). A probabilistic link may be another person's: never act on it alone.
  - A deferred link is delivered through `OnLinkReceived` exactly once, never by
    `GetInitialLink`, without an `app_open` event; a normal link wins over it; nothing is
    reported through `OnLinkError`. The two stored values (`attempts`, `done`) live in
    `Saved/Strybok/DeferredLinks.txt`; the referrer, the pasted text and the click id are never
    stored or logged.
  - Core: `Config` gains `deferredLinks`, `allowProbabilisticMatching`, `installReferrerSources`,
    `androidPackageName`, `iosBundleId`, `deviceFamily`, `locale`, `pasteboardMayContainLink` and
    `deferredLinkStore` (`IDeferredLinkStore`); `LinkEngine` gains `GetDeferredLinkStatus`,
    `SetDeferredLinkStatusHandler`, `EnableDeferredLinks`, `SetInstallReferrerSources`,
    `RedeemPastedLink`, `SkipPastedLink`; `ResolvedLink` gains `isDeferred` and
    `isProbabilisticMatch`; pure functions `FindClickIdInReferrer`, `FindClickIdInPastedLink`,
    `BuildAndroidRedeemBody`, `BuildIosRedeemBody`, `BuildMatchBody`, `ParseDeferredBody` and
    friends. The whole flow (attempts stored before a source is asked, three attempts, `done`
    before delivery, a normal link wins, pasted link, probabilistic matching, every status) is
    covered by the standalone tests.
  - Not compiled against Unreal Engine and not run on a device or against a real store; the
    Huawei and Samsung flows are unverified even in the native Android SDK.
- Which URLs are accepted, duplicate detection (the whole URL string as received) and everything
  else are unchanged. `Version` in `Strybok.uplugin` is now 3.
- The Unreal module part of this (`FStrybokCampaign`, the conversion in `StrybokRuntime.cpp`,
  the Blueprint function) is, like the rest of the module, not compiled against Unreal Engine
  yet.

## 0.2.0

- `UStrybokSubsystem::CreateLink` (Blueprint) and `CreateLinkNative` (C++ lambda): create a short
  link to a screen of the game for a "Share" button. Needs the new `ClientKey` setting
  (publishable key `pk_…`; also in `FStrybokConfig`). Completes with an `FStrybokCreatedLink`
  (`Url`, `Slug`, `RoutePath`, `LinkId`) or an `FStrybokError`. Works without `Configure`.
- `EStrybokErrorKind` gains `Unauthorized`, `RouteNotAllowed`, `RateLimited` and `Validation`;
  `FStrybokError` gains `RoutePath`, `RetryAfterSeconds` and `Issues`.
- Core: `strybok::LinkCreator` with the pre-flight checks, the request, response handling,
  timeout and retries (same body on every attempt) of the SDK contract; `HttpResponse::retryAfter`;
  `Config::clientKey`. Covered by the standalone tests.
- The Unreal module part of this (subsystem, settings, `Retry-After` from `FHttpModule`) is, like
  the rest of the module, not compiled against Unreal Engine yet.

## 0.1.0

- Initial version. Not compiled against Unreal Engine or run on a device yet; only the
  engine-independent core is tested.
- `UStrybokSubsystem`: `Configure`, `ConfigureFromSettings`, `OnLinkReceived`, `OnLinkError`,
  `GetInitialLink`, `TryGetInitialLink`, `ResolveUrl`, `HandleUrl`, `IsStrybokUrl`, `Shutdown`.
- `UStrybokSettings` in Project Settings → Plugins → Strybok.
- `FStrybokResolvedLink` with payload and route helpers, `FStrybokError` with typed kinds.
- Core (`StrybokCore`): host allowlist; duplicate-delivery protection; timeout and retry with
  back-off; response validation; exactly-once queue; best-effort `app_open` events; standalone
  tests.
- iOS: Universal Links through an app-delegate hook. Android: App Links intent filter and
  `GameActivity` glue through UPL.
