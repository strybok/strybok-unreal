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
