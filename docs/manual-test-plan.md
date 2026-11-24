# Manual device test plan

Automated tests cover the SDKs and the Flutter example app with simulated links. Whether iOS and Android
really hand a link to the app can only be checked on devices, with a real domain and real signing
identities.

> **Results: not executed yet.** No public domain, Apple Team or Play signing key was available
> when the SDKs were written. Do not treat the device behaviour as confirmed until the table
> below is filled in.

## Build checks done so far (2026-11-11, macOS, Flutter 3.44.0)

| Check                                                                            | Result                                                                                                                                                                                                                                                                                                                                                                                                           |
| -------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `flutter build apk --debug` for the Flutter example app (`example/` in github.com/strybok/strybok-flutter) | Passed. The built manifest contains the `autoVerify` intent filter and `flutter_deeplinking_enabled=false`.                                                                                                                                                                                                                                                                                                      |
| `flutter build ios --simulator --debug`                                          | **Failed** in Flutter's own "Prepare Flutter Framework" step (`Flutter.framework … does not contain architectures "arm64 x86_64"`). A freshly generated, unmodified `flutter create` app fails identically on this machine (Xcode 27.0), so this is a toolchain incompatibility, not project code. The iOS project has therefore **not been compiled yet**; retry with a Flutter release that supports Xcode 27. |

## Preconditions

- A reachable Strybok API with wildcard DNS/TLS for the links base domain.
- An application with real `iosTeamId`/`iosBundleId` and `androidPackageName`/fingerprints.
- The Flutter example app (`example/` in github.com/strybok/strybok-flutter) with that hostname in `AndroidManifest.xml`,
  `Runner.entitlements` and `--dart-define=STRYBOK_HOST=… --dart-define=STRYBOK_API=…`;
  bundle/application IDs changed to yours.
- A link `https://<hostname>/<slug>` with `routePath` `/invite/<code>`.
- Both association files return 200 JSON; `adb shell pm get-app-links` shows `verified`.

Tap links from Notes/Messages (not the browser address bar).

## Scenarios

| #   | Scenario                                                | Expected                                                  | iOS (device, OS, date, result) | Android (device, OS, date, result) |
| --- | ------------------------------------------------------- | --------------------------------------------------------- | ------------------------------ | ---------------------------------- |
| 1   | App installed, app not running (cold start)             | App opens on the invite screen, once                      | —                              | —                                  |
| 2   | App in background                                       | App resumes on the invite screen, once                    | —                              | —                                  |
| 3   | App in foreground (link from a notification/split view) | Invite screen opens once                                  | —                              | —                                  |
| 4   | Signed out, cold start with link                        | Login shown; invite screen after signing in               | —                              | —                                  |
| 5   | Same link tapped twice, a minute apart                  | Opens each time                                           | —                              | —                                  |
| 6   | App not installed                                       | Browser → store (or configured fallback)                  | —                              | —                                  |
| 7   | Association broken (wrong fingerprint / Team ID)        | Browser fallback; no app open                             | —                              | —                                  |
| 8   | Disabled link, app installed                            | App opens, stays where it is; `onError` reports not found | —                              | —                                  |
| 9   | Disabled/expired link, app not installed                | "Link unavailable" page (410)                             | —                              | —                                  |
| 10  | Device offline, app installed                           | App opens; no navigation; no crash                        | —                              | —                                  |
| 11  | Link inside WhatsApp                                    | Record actual behaviour                                   | —                              | —                                  |
| 12  | Link inside Telegram                                    | Record actual behaviour                                   | —                              | —                                  |
| 13  | Link inside Instagram                                   | Record actual behaviour                                   | —                              | —                                  |
| 14  | AASA changed (iOS)                                      | Record time until devices pick it up                      | —                              | n/a                                |

For every failure note the OS version, where the link was tapped, and the output of
`adb shell pm get-app-links <package>` (Android) or the Apple CDN response (iOS).
