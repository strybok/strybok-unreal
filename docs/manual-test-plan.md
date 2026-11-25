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

## Deferred links (decision 0019)

> **Results: not executed.** The scenarios can be run now — the Android and Kotlin Multiplatform
> SDKs 0.4.0 (unreleased) implement deferred links with the three store sources — but none has
> been: so far the feature is covered by unit tests only, and the server side is not deployed to
> production. D7, D8, D15, D19, D20 and D24 can also be run with a test app that only prints
> what the store's install referrer returns, and they decide whether the Huawei and Samsung
> rows of the store table are right.

Preconditions: `deferredLinksEnabled` on; an app build with `deferredLinks` on and the store's
referrer library, **published in the store under test** (record which track); the app not
installed; a debug screen showing `deferredLinkStatus` and, for the recording scenarios, the raw
referrer string.

| #   | Scenario                                                                                                                     | Expected                                                                          | Result (device, OS, store app version, date) |
| --- | ---------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- | -------------------------------------------- |
| D1  | Tap link in Messages → Chrome → Play Store app → Install → Open from the Play Store                                          | Invite screen opens once; status `delivered`                                      | —                                            |
| D2  | As D1, but open the app from the launcher                                                                                    | Same                                                                              | —                                            |
| D3  | As D1, first launch 2 hours after install                                                                                    | Same                                                                              | —                                            |
| D4  | As D1, first launch more than 24 hours after the tap                                                                         | Home screen; status `notFound`                                                    | —                                            |
| D5  | As D1, device offline at first launch, online at the second                                                                  | Home screen first (`pending`); invite screen on the second launch                 | —                                            |
| D6  | Tap link, leave the listing, later find the app by searching in Play                                                         | Record whether the referrer survives; expected `noReferrer`                       | —                                            |
| D7  | Tap link inside Instagram / Facebook / Telegram / WhatsApp in-app browsers                                                   | Record: store app or store website? referrer present?                             | —                                            |
| D8  | As D7 with a test link that redirects to `market://details?id=…&referrer=…`                                                  | Record the behaviour (input for a later decision; not shipped)                    | —                                            |
| D9  | Install from a Play internal test track / internal app sharing                                                               | Record whether a referrer is delivered                                            | —                                            |
| D10 | Tap a normal link of the app right after install, before the first launch                                                    | That link opens once; status `superseded`; no second navigation                   | —                                            |
| D11 | First launch delivered; force-stop; launch again                                                                             | No navigation; status `notFirstLaunch`                                            | —                                            |
| D12 | Delivered; clear app data; launch again                                                                                      | No navigation (id already redeemed); status `notFound`                            | —                                            |
| D13 | Uninstall, reinstall from the launcher's Play shortcut without tapping a link                                                | Record what referrer Play returns; expected no navigation                         | —                                            |
| D14 | Existing installation updates to the first app version with the feature                                                      | No check, no navigation; status `notFirstLaunch`                                  | —                                            |
| D15 | Store URL with the owner's own `referrer` and campaign parameters, total over 300 characters                                 | Link delivered; record the string the app received                                | —                                            |
| D16 | Device without the store app (or with it disabled)                                                                           | App starts normally; status `unavailable`                                         | —                                            |
| D17 | Link disabled between tap and first launch                                                                                   | Home screen; status `notFound`                                                    | —                                            |
| D18 | Measure app start time with and without `deferredLinks`                                                                      | No measurable difference before the first frame                                   | —                                            |
| D19 | Huawei device: country rule or Android fallback to `https://appgallery.cloud.huawei.com/appDetail?pkgName=…` → install       | Record the exact string in provider column 5 (encoded? complete?); link delivered | —                                            |
| D20 | Samsung device: Android fallback to `https://apps.samsung.com/appquery/appDetail.as?appId=…` → install                       | Record: did Galaxy Store open directly or through the web page? referrer string?  | —                                            |
| D21 | As D20 with a test redirect to `samsungapps://ProductDetail/<package>?referrer=…`, and D19 with `hiapplink://…`              | Record the behaviour (input for a later decision; not shipped)                    | —                                            |
| D22 | App with all three referrer sources, installed from Google Play on a Samsung device                                          | Play's referrer is used; the other sources answer without delay                   | —                                            |
| D23 | Landing page ("always show the landing page"): tap "Get it on Google Play"; separately tap "Open in the app" without the app | Both end in the Play Store app with the referrer; link delivered                  | —                                            |
| D24 | AppGallery: read the provider without the manifest `<queries>` entry and permission, then with them                          | Record which is needed on Android 11+                                             | —                                            |

### iOS options

> **Results: not executed.** The scenarios can be run now — the iOS SDK (Swift) and Kotlin
> Multiplatform 0.4.0 (unreleased) implement both flows — but none has been: unit tests only, no
> device, no real pasteboard. I10–I17 additionally need a service with the matching secret set,
> which the hosted production service does not have today. I3, I8 and I9 can be run with Safari
> alone (the page and the clipboard); the rest need an app.

Preconditions: an application with an iOS bundle ID and App Store URL; for I3–I9
`iosDeferredClipboard` on and an app with a paste control wired to `redeemPastedLink`; for
I10–I14 `iosDeferredMatching` on, the link marked "allow probabilistic matching", and an app
with `allowProbabilisticMatching`.

| #   | Scenario                                                                                            | Expected                                                                                  | Result (device, iOS, date) |
| --- | --------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- | -------------------------- |
| I3  | Clipboard: Safari → page → Copy link and get the app → install → open → Paste                       | App Store **app** opens after the copy; invite screen opens; no system alert at any point | —                          |
| I4  | I3 without tapping Paste                                                                            | Nothing is read; no alert; app stays on its first screen                                  | —                          |
| I5  | I3 with other text copied in between                                                                | Paste gives `notOurs`; nothing is sent; no navigation                                     | —                          |
| I6  | I3 with a second Apple device nearby (Universal Clipboard)                                          | Record whether the link appears there                                                     | —                          |
| I7  | Apple's own option: custom product page with a deep link (iOS 18+): install → Open in the App Store | Record universal link versus custom scheme behaviour                                      | —                          |
| I8  | Clipboard page inside Instagram / Facebook / TikTok in-app browsers, and in Chrome for iOS          | Record: does the copy button appear, does the copy succeed, where does the store link go  | —                          |
| I9  | Clipboard page with content blockers / script disabled                                              | Plain store button and the link as text; manual copy and paste works                      | —                          |
| I10 | Matching: Safari tap on Wi-Fi → install → first launch on the same Wi-Fi within the window          | Link delivered with `isProbabilisticMatch`; record how long after the tap                 | —                          |
| I11 | I10 with iCloud Private Relay on; and with a VPN app on                                             | No match (`notFound`); nothing delivered                                                  | —                          |
| I12 | I10 switching from Wi-Fi to mobile data between tap and launch; and on mobile data throughout       | Record: no match in the first case; in the second, whether the carrier keeps the address  | —                          |
| I13 | Two phones on the same Wi-Fi tap two allowed links; either installs and launches                    | Neither gets a link; "Ambiguous, refused" counts for both links                           | —                          |
| I14 | One phone taps an allowed link; a **different** phone on the same Wi-Fi installs and launches first | That phone gets the first person's link — the documented failure. Record that it happens  | —                          |
| I15 | iPad with default Safari settings taps an allowed link                                              | Record the User-Agent class; expected: treated as desktop, no candidate                   | —                          |
| I16 | iOS 26 or later: compare the OS version in Safari's User-Agent with the real one                    | Record (decides whether the OS version could ever join the signal)                        | —                          |
| I17 | Both options on: tap, copy, install, paste                                                          | Pasted link delivered once; no second navigation from matching                            | —                          |

There are no scenarios for a Smart App Banner page: it is not built.
