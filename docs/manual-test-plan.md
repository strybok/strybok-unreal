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
> SDKs 0.4.0 (released 2026-11-25) implement deferred links with the three store sources, and
> the server side is deployed to production — but none has been: so far the feature is covered
> by unit tests only. D7, D8, D15, D19, D20 and D24 can also be run with a test app that only prints
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
> Multiplatform 0.4.0 (released 2026-11-25) implement both flows — but none has been: unit tests only, no
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

## Custom domains (decision 0020)

Run on production on 2026-11-25 with `strybok-test.stepaniuk.dev` (its DNS is on Cloudflare, in
the same account as the `strybok.link` zone; CNAME "DNS only") and a test application with
made-up identifiers. Scenarios that need an app build, a device or a second workspace are still
open. Needs the one-time setup in
deployment.md, a hostname you control (for
example `go.<your domain>`) and an app build that declares it.

**Since 2026-11-27 the flow is different** (decision 0020, amendment): a claim reaches Cloudflare
only once its TXT record has been seen, so Cloudflare's validation starts at that check, not when
the CNAME is created. The 2026-11-25 results below were obtained with the previous flow, where the
hostname was created at Cloudflare at claim time; the amended flow has **not been run yet**. Watch
**SSL/TLS → Custom Hostnames** of the `strybok.link` zone while running C1, C1a and C1b.

| #   | Scenario                                                                                                            | Expected                                                                                                                                                                                                                                                                                        | Result                                                                                                                                                                                                                                                                                                                             |
| --- | ------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| C1  | Add the hostname, create the TXT record and the CNAME, refresh until `active`; record how long each took to be seen | Nothing in Custom Hostnames until a check finds the TXT record; that check creates the hostname, Cloudflare reports it active a few minutes later (about 2 + 2 on 2026-11-25), counted from that check; then `https://<host>/.well-known/*` return 200 JSON identical to the managed hostname's | Previous flow ✅ 2026-11-25: TXT found on the first check; Cloudflare said "does not CNAME to this zone" right after the CNAME, hostname active about 2 min later, certificate (HTTP) about 2 min after that; files identical, 200 JSON, no redirect; `/hello` 302, `resolve` accepted the host, unknown slug 404. Amended flow: — |
| C1a | Create only the CNAME (no TXT record) and wait a few checks                                                         | Stays `pending`, "Waiting for the TXT record"; never appears in Custom Hostnames (no certificate is issued); nothing served; removed after 72 hours                                                                                                                                             | —                                                                                                                                                                                                                                                                                                                                  |
| C1b | Second workspace: claim a hostname whose old CNAME (and old TXT record) still point at us                           | Stays `pending` with "wrong value"; never appears in Custom Hostnames; never served; the real owner can claim it after it expires                                                                                                                                                               | —                                                                                                                                                                                                                                                                                                                                  |
| C1c | Look the TXT name up before creating the record, then create it                                                     | Record how long the negative cache delays "found"                                                                                                                                                                                                                                               | —                                                                                                                                                                                                                                                                                                                                  |
| C2  | Customer DNS on Cloudflare: CNAME as "DNS only", and separately proxied                                             | Record which one validates; the TXT record works in both                                                                                                                                                                                                                                        | Partly, 2026-11-25: "DNS only" validated (hostname and certificate active); proxied not tried. Visitors got the zone's own certificate (`*.stepaniuk.dev`, same Cloudflare account), not the one Cloudflare for SaaS issued                                                                                                        |
| C3  | App build without the custom domain: tap `https://<host>/<slug>`                                                    | Browser fallback (no app); the managed hostname still opens the app                                                                                                                                                                                                                             | —                                                                                                                                                                                                                                                                                                                                  |
| C4  | App build with both hosts (entitlement, intent filter, `allowedHosts`): tap links on both hostnames                 | The app opens for both; `adb shell pm get-app-links` lists both hosts as `verified`                                                                                                                                                                                                             | —                                                                                                                                                                                                                                                                                                                                  |
| C5  | Android 11 or lower, build that declares the custom domain while it is still pending                                | Record: App Links verification fails for the managed hostname too (why only active ones are added)                                                                                                                                                                                              | —                                                                                                                                                                                                                                                                                                                                  |
| C6  | Make it primary; create a link in the dashboard and with `createLink`                                               | Both URLs use the custom domain; old managed URLs keep working                                                                                                                                                                                                                                  | Partly, 2026-11-25: the dashboard's link URL and QR code used the custom domain and the managed URL kept redirecting; `createLink` not tried                                                                                                                                                                                       |
| C7  | Remove the domain, keep both records                                                                                | The hostname stops answering (Cloudflare's own error page); record what a visitor sees                                                                                                                                                                                                          | ✅ 2026-11-25: Cloudflare listed no custom hostnames; the host answered Cloudflare error 522; `resolve` refused its URLs. Both records were then deleted                                                                                                                                                                           |
| C8  | Add a hostname and remove it before creating any record                                                             | Removed at once (204); nothing was ever in Custom Hostnames                                                                                                                                                                                                                                     | —                                                                                                                                                                                                                                                                                                                                  |

## Diagnostics and the link simulator (decision 0030)

> **Results: not executed.** Built and tested against the local database and local fakes of
> Apple's CDN and Google; the Worker has never asked the real services. Needs an application
> whose iOS and Android identifiers belong to a real app build, and that build on a phone.

| #   | Scenario                                                                                      | Expected                                                                                                                             | Result |
| --- | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ | ------ |
| DG1 | Setup → "Check with Apple and Google"; compare with `curl -i` of Apple's CDN URL for the host | The Apple check states what curl shows (copy or failure reason, `max-age`); Google's answer matches `assetlinks:check` asked by hand | —      |
| DG2 | Change the Team ID, run again at once and again hours later                                   | First "Apple's copy is older" with how long it may stay, later "OK"; record how long Apple took                                      | —      |
| DG3 | Add a fingerprint, run again; then `adb shell pm get-app-links <package>` on the phone        | Google first "does not link … (yet)", then "OK"; record how long Google and the phone took                                           | —      |
| DG4 | "Test this link" for Safari, Instagram (iOS and Android) and Chrome, app installed or not     | What the phone does matches step 1, the page or redirect matches step 2, and the app gets the route and payload of step 3            | —      |

## iOS in-app browsers (decision 0034)

> **Results: not executed.** Built and tested against the local Worker only, with sample
> User-Agents; no iPhone and no in-app browser was involved. Needs an iOS build whose Associated
> Domains list the app's hostname and its alternate hostname (`<label>--alt.<base>`, shown on the
> setup screen), whose SDK has both in `allowedHosts`, and "Open in the app" for iPhone switched
> on. Post the link in a story, a post, a comment or a direct message of each app, as people
> would.

| #   | Scenario                                                                           | Expected                                                                                                                   | Result |
| --- | ---------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ------ |
| IA1 | Instagram (iOS), app installed: tap the link, then "Open in the app"               | Record what happens. Branch documents that Instagram never hands such a tap to iOS: then the same page on the other host   | —      |
| IA2 | Facebook and Messenger (iOS), app installed: the same                              | Expected from Branch's documentation: the app opens on the link's screen; record it                                        | —      |
| IA3 | X, LinkedIn, WeChat (iOS), app installed: the same                                 | Record it; X may not be recognised at all (Safari view) and then redirects as in Safari                                    | —      |
| IA4 | TikTok, Snapchat, LINE (iOS), app installed: the same                              | Unknown: record it                                                                                                         | —      |
| IA5 | Any of them, app **not** installed: "Open in the app"                              | The same page on the other hostname (button back, App Store button); never a loop that moves by itself                     | —      |
| IA6 | An app version without the alternate hostname in its entitlement                   | The same page on the other hostname; the app does not open                                                                 | —      |
| IA7 | After IA1–IA4: the link's analytics                                                | Each tap the browser kept is one "Visit on the alternate hostname" and a second browser fallback; app opens are `app_open` | —      |
| IA8 | Apple's CDN: `curl -i https://app-site-association.cdn-apple.com/a/v1/<alternate>` | 200 with the app's App ID once a device installed the build; Diagnostics show the same                                     | —      |
| IA9 | Safari and Android with the switch on                                              | Unchanged: Safari opens the app or the App Store as before; Android's page has its `intent:` button                        | —      |

## Team members and invitations (decision 0023)

> **Results: not executed.** Built and tested against the local database with the dev sign-in
> and with locally signed tokens; never tried with real Supabase accounts or in production. Needs
> two Supabase accounts with addresses you can read, in a throwaway workspace.

| #   | Scenario                                                                                                 | Expected                                                                                                                                                     | Result |
| --- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------ |
| T1  | Owner invites the second account's address as **Member**; in a private window, open the link             | Sign-in page with the invitation note; after signing in (or creating and confirming the account) the page names the workspace and "a member"; **Join** works | —      |
| T2  | Before T1's Join: sign in with a third account, or invite with a different capitalisation of the address | Third account: "for another email address", without naming it. Capitalisation: accepted (the `email` claim is compared lower-cased)                          | —      |
| T3  | As the member: Apps, Setup, Links, Members, API keys, Workspace                                          | Apps read-only, links can be created and edited, no Members or API keys pages, no export or deletion; the API answers 403 to a key creation                  | —      |
| T4  | Owner: open the used link again; revoke a second invitation and open its link; let one expire (or SQL)   | "already been used" / "revoked" / "expired"; a day after expiry the link says "does not work" and the row is gone                                            | —      |
| T5  | Owner transfers ownership to the member; the former owner leaves                                         | New owner sees export and deletion; the former owner is an admin, then gone; the activity lists every step, current members by address                       | —      |
| T6  | Before T5: the owner tries **Leave and delete my data**; check the members page                          | Refused: the page says they are the only owner and offers to transfer or delete; the members page shows the member's real address                            | —      |

## Workspace export and deletion (decision 0022)

> **Results: not executed.** Built and tested against the local database and a fake Cloudflare;
> never tried in production. Use a throwaway workspace: deletion cannot be undone from the
> dashboard.

| #   | Scenario                                                                                                                    | Expected                                                                                                                                                                                                                       | Result |
| --- | --------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------ |
| W1  | Workspace with an app, links (one with campaign defaults), an API key and an active custom domain: **Export**               | A file `strybok-<name>-<day>.json`, `"format": "strybok-export/1"`, `"complete": true`; everything the dashboard shows is in it; searching it finds no `ciphertext`, hash or `key_hash`                                        | —      |
| W2  | Same workspace: **Delete workspace** with the exact name; then open a link on the managed hostname and on the custom domain | Both stop at once (404 page; the custom domain soon gives Cloudflare's own error); Custom Hostnames no longer lists it; for every table no row with the workspace's ids is left; `released_hostnames` has the managed hostname | —      |
| W3  | **Leave and delete my data** with one workspace owned alone and, if possible, one shared with a second owner                | Signed out; the owned one is gone, the shared one stays with the other owner; signing in again works and shows no workspace. Ask hello@strybok.com to delete the sign-in record; record how long it took                       | —      |
| W4  | On staging only: note the time, delete a test workspace, then restore D1 Time Travel to before the deletion                 | The workspace comes back (why a restore must be followed by deleting again what had been deleted after the restore point)                                                                                                      | —      |

## Web banner on a website (decision 0024)

> **Results: not executed.** Built and tested with a simulated DOM, the local Worker and Chrome
> with iPhone and Android user agents; never on a device, in Safari or in an in-app browser. Needs
> a test page on a public HTTPS site with the script tag, an app build with Universal Links / App
> Links working for the link host, and a link of that app.

| #   | Scenario                                                                                                 | Expected                                                                                                      | Result |
| --- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- | ------ |
| B1  | iPhone, Safari, app installed: open the page, tap **Open**                                               | The app opens at the link's screen; the SDK's `app_open` carries `utm_source=website`, `utm_medium=banner`    | —      |
| B2  | iPhone, Safari, app not installed: tap **Open**                                                          | The link's iOS fallback (App Store); the link's analytics count a browser fallback with source `website`      | —      |
| B3  | Android, Chrome, app installed and App Links verified: tap **Open**; then the same without the app       | The app opens at the link's screen; without it the Play Store (with the referrer when deferred links are on)  | —      |
| B4  | iPad with default settings (desktop user agent), and a desktop browser                                   | iPad: the banner shows. Desktop: nothing at all                                                               | —      |
| B5  | Close the banner, reload, open another page of the site; change the device date past `dismissDays`       | Stays hidden on the site; back after the days have passed                                                     | —      |
| B6  | Safari private browsing, and with content blockers on                                                    | Private: shows, closing lasts for the page view. Blockers: record whether the script or the banner is blocked | —      |
| B7  | The page inside Instagram / Facebook / TikTok in-app browsers and Chrome for iOS: tap **Open**           | Record where each ends up (the app, the store, the page)                                                      | —      |
| B8  | A single-page app changes `strybok:link` on navigation; tap on the second page                           | The second page's link opens                                                                                  | —      |
| B9  | The site's own strict CSP (`script-src` the link host only, no `'unsafe-inline'` for styles), Safari 17+ | Banner shown with its styles; nothing in the console but what the page itself causes                          | —      |
| B10 | Light and dark mode, landscape, a phone with a notch, VoiceOver and TalkBack                             | Readable in both; clear of the notch; announced as a region with the title, a link and "Close"                | —      |

## API usage (decision 0027)

> **Results: not executed.** Built and tested against the local database and in a local browser;
> never tried in production. Use a throwaway key with `links:read` and `links:write` only.

| #   | Scenario                                                                                                                                               | Expected                                                                                                                   | Result |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------- | ------ |
| U1  | Three requests with the key that work (`GET /v1/me`, a link list, a link creation) and one it lacks the scope for (`GET /v1/apps/:id`); open **Usage** | Today: 3 succeeded and 1 client error for that key, the same in the chart's tooltip, the day's table and the per-key table | —      |
| U2  | The same key writes 130 times within a minute (`PATCH` one link with the same body)                                                                    | The writes over 120 are refused with 429 and counted as rate limited                                                       | —      |
| U3  | Revoke the key, use it once more, reload **Usage**                                                                                                     | The 401 is not counted; the key stays listed as revoked, with its numbers                                                  | —      |
| U4  | As a member of the workspace                                                                                                                           | No **Usage** entry; `/settings/usage` says it is for owners and admins                                                     | —      |
| U5  | Next day, Cloudflare dashboard → D1 → the database's metrics                                                                                           | Rows written grew by about the number of counted requests; nothing else unexplained                                        | —      |

## Webhooks (decision 0025)

> **Results: not executed.** Built and tested with a fake receiver and, in the browser test, a
> receiver on the developer's machine; not deployed, and no webhook has ever been delivered to a
> real endpoint on the internet. Needs a public HTTPS endpoint you control that shows the requests
> it receives (a small server of your own is best: the bodies carry your link data).

| #   | Scenario                                                                                                                                                                 | Expected                                                                                                                                                                                                                                                 | Result |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| WH1 | Add an endpoint for all five events; copy the secret; **Send test event**                                                                                                | "Delivered: HTTP 200 …"; the endpoint received `webhook.test` with `webhook-id`, a `webhook-timestamp` within seconds of now and one `webhook-signature`, which the code of `docs/webhooks.md` verifies with the secret                      | —      |
| WH2 | Create a link in the dashboard; open it in a desktop browser, on an Android phone (store) and inside Instagram; open it once with the app installed (the SDK reports it) | `link.created`; three `link.clicked` with `fallback_visit`, `store_redirect` and `in_app_browser_view`; a `link.app_opened` with `reportedBy: "app"`; no body contains an IP address or User-Agent; redirects are as fast as before; record the CPU time | —      |
| WH3 | Make the endpoint answer 500 (or 404); trigger an event; fix the endpoint                                                                                                | The delivery shows "Retrying" with the next attempt about 5 minutes later; the retry arrives 5–10 minutes after the first attempt; once fixed, the next retry succeeds and the endpoint is Active again; Workers Logs shows the retry run's counts       | —      |
| WH4 | **Rotate secret**; send a test event; send another more than 24 hours later                                                                                              | First: two signatures, the new secret's first, both verify; later: only the new one                                                                                                                                                                      | —      |
| WH5 | Try to add `https://api.strybok.com/x`, `https://<app>.strybok.link/x`, an IP address and an `http://` URL                                                               | Each refused with a message under the URL field; nothing is sent                                                                                                                                                                                         | —      |
