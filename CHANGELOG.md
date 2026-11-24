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
