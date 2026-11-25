# iOS Universal Links setup

Goal: iOS opens `https://<your managed hostname>/<slug>` directly in your app.

## 1. Tell Strybok about the app

Set on the application (`PATCH /v1/apps/:appId`):

- `iosTeamId` — Apple Developer Team ID (10 characters, _Membership details_ in the developer account)
- `iosBundleId` — the app's bundle identifier
- `iosStoreUrl` — your App Store URL (used as the fallback)

Strybok then serves `https://<hostname>/.well-known/apple-app-site-association`. Check it:

```sh
curl -i https://<hostname>/.well-known/apple-app-site-association
```

Expected: `200`, `Content-Type: application/json`, no redirect, and
`"appIDs": ["<TEAMID>.<bundle id>"]`. An empty `details` array means the Team ID or bundle ID is
still missing.

## 2. Add the Associated Domains capability

In Xcode: _Runner target → Signing & Capabilities → + Capability → Associated Domains_, then add

```text
applinks:<your managed hostname>
```

This creates `Runner.entitlements` with:

```xml
<key>com.apple.developer.associated-domains</key>
<array>
    <string>applinks:<your managed hostname></string>
</array>
```

The capability must also be enabled for the App ID in the Apple Developer portal (Xcode's
automatic signing does this). No `https://`, no path, exact hostname.

Flutter only — disable the built-in handler in `ios/Runner/Info.plist`, because `app_links`
handles links:

```xml
<key>FlutterDeepLinkingEnabled</key>
<false/>
```

## 3. Verify on a device

- Use a **physical device**. Install the build, then tap a link in Notes or Messages.
  Typing the URL into Safari's address bar does not trigger Universal Links.
- Long-press the link: an "Open in <App>" entry means the association is active.
- iOS fetches the association file through **Apple's CDN**, not from Strybok directly. After
  changing identifiers it can take hours before devices see the new file. See what Apple has:

  ```sh
  curl -i https://app-site-association.cdn-apple.com/a/v1/<hostname>
  ```

- For development builds you can bypass the CDN: add `?mode=developer` to the entitlement
  (`applinks:<hostname>?mode=developer`) and enable _Settings → Developer → Associated Domains
  Development_ on the device. Do not ship this to the App Store.
- If the user once chose to open the link in Safari (by tapping the breadcrumb at the top right),
  iOS remembers it; long-press the link and choose "Open in <App>" to switch back.

Strybok can only tell you that the file is served correctly. Whether iOS associated the domain
is visible only on the device.

## Troubleshooting

| Symptom                                          | Likely cause                                                                                                                                            |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Link always opens Safari                         | Entitlement missing or wrong hostname; Team ID/bundle ID mismatch; Apple CDN still has the old file; the link was tapped in the same domain's web page. |
| Works in Notes, not in Instagram/TikTok/Facebook | See [Links opened from Instagram, TikTok and other apps](#links-opened-from-instagram-tiktok-and-other-apps).                                           |
| Works in a development build only                | The App Store build is signed for a different Team ID or bundle ID than configured.                                                                     |

Changing the hostname in the entitlement requires shipping an app update.

### Links opened from Instagram, TikTok and other apps

Instagram, Facebook, Messenger, TikTok, LinkedIn, Snapchat, LINE, WeChat and similar apps open
links in their own built-in browser. iOS does not hand the link to your app there, even when it
is installed. This is how those apps work, not a fault in your setup.

When Strybok recognises such a browser (best-effort, from the User-Agent) it shows a page instead
of redirecting to the App Store: the link's title and description, the App Store (or fallback)
button, and a line telling the visitor to choose "Open in Safari" / "Open in browser" from the
app's menu.

**There is no "Open in the app" button on iOS.** A page cannot open your app from the link's own
domain: Apple opens same-domain links in the browser, and the SDKs do not use custom URL schemes.
Leaving the built-in browser through its menu is the only route offered, and whether that lands
in your app or in Safari depends on the host app. X on iOS is probably not recognised at all and
is redirected as before.

None of this has been verified inside the real apps or on a device. Details and the options that
were not taken: decision 0017.

## Links tapped before the app is installed

By default **nothing happens**: if a person taps a link, installs the app from the App Store and
opens it, the app starts on its first screen. That is Apple's platform, not a missing feature —
the App Store passes nothing from a link to a newly installed app, so everything below is a
trade-off, and **there is no automatic deferred deep linking on iOS**. Nothing here has been
verified on a device, with a real pasteboard or against the production service. The app side is
implemented, in versions that are not released yet, by the **iOS SDK (Swift) 0.4.0** — see
[Deferred links (optional)](https://github.com/strybok/strybok-ios) — and by Kotlin
Multiplatform 0.4.0; Flutter, JavaScript, React Native, Capacitor, Unity, .NET and Unreal 0.3.0
implement the flow but your app supplies the pasted text (and, for matching, the device family
and language). See
[which SDKs implement what](./sdk-contract.md#which-sdks-implement-deferred-links); what SDKs do
is in the [SDK contract](./sdk-contract.md#deferred-links-on-ios-optional).
Background and sources:
decision 0019.

### Recommended first: Apple's own mechanisms

They need nothing from Strybok and cost your users nothing.

- A **custom product page** in App Store Connect with an app deep link. Use that page's URL as
  the link's iOS fallback. It applies on iOS 18 and later, only when the person taps **Open** in
  the App Store, for a limited number of fixed destinations, each reviewed by Apple.
- An **App Clip** that keeps its invocation URL for the full app.

### Option: copy and paste the link (`iosDeferredClipboard`)

Off by default. Dashboard: **iOS: links tapped before the app is installed → Copy and paste the
link**, or `PATCH /v1/apps/{appId}` with `{ "iosDeferredClipboard": true }`. Needs the iOS
bundle ID.

How it works:

1. Instead of the redirect to the App Store, iOS visitors see a page with a **Copy link and get
   the app** button and a sentence saying that it replaces their clipboard. Tapping it copies
   `https://<your link host>/<slug>?strybok_click=dc_…` and opens the App Store. The page also
   has a plain store button ("Get the app without copying") and shows the link as text, so it
   works without script.
2. In your app, on the first screen or in onboarding, you show a paste button — Apple's
   `UIPasteControl` (iOS 16 or later; no permission alert) — and pass what the person pasted to
   the SDK's `redeemPastedLink`. The SDK never reads the clipboard itself. If the pasted text is
   a link of your app with a valid code, the SDK exchanges the code once and delivers the link
   like any other, marked `isDeferred`.

What it costs:

- **Every** iOS visitor without the app gets one extra page and one extra tap before the App
  Store, whether or not they would ever paste.
- It only works if the person taps the copy button and later taps Paste in your app. Nobody
  publishes how many people do; expect a minority.
- It replaces whatever was on their clipboard, and Apple's Universal Clipboard may carry the
  link to their other devices.
- The code works once, for 24 hours. A person who copies something else in between gets
  nothing.
- You have to build the paste step into your onboarding and describe it in your App Review
  notes.
- The page is the only Strybok page that runs a script (one static file from your link host,
  under a Content-Security-Policy that allows nothing else). Whether in-app browsers allow the
  copy, and whether Safari still opens the App Store app after it, is unverified.

### Option, not recommended: probabilistic matching (`iosDeferredMatching`)

Off by default, and we advise against it. It exists because some owners want the choice other
services make for them silently.

How it works: when an iOS visitor of an **allowed** link is sent to the App Store, Strybok keeps
a keyed hash of their IP address, device type (iPhone/iPad) and language for a short time
(15 minutes, 1 hour — the default — or 6 hours). When your app starts for the first time, the
SDK asks whether exactly one such tap came from the same address, device type and language. If
so, it delivers that link, marked `isDeferred` and `isProbabilisticMatch`. The person sees
nothing.

To enable it, all of these are needed: the server has a matching secret (the dashboard says so
if not — **the hosted service does not have one today, so matching cannot be switched on
there**); you switch it on under **iOS: links tapped before the app is installed** and confirm
the warning (`{ "iosDeferredMatching": true, "iosMatchingWindowMinutes": 60 }`); you tick
**Allow probabilistic matching on iOS for this link** on each link that may be matched
(`allowProbabilisticMatch`; links your app creates itself can never be); and you set
`allowProbabilisticMatching` in the SDK.

What it costs — read all of it:

- **Apple forbids it.** The App Store Review Guidelines and the Apple Developer Program License
  Agreement do not allow deriving data from a device to identify it (fingerprinting), with or
  without the user's consent. If you enable this, the App Review and legal risk is yours, not
  Strybok's.
- **A wrong match shows one person another person's link.** People behind one address — shared
  Wi-Fi, offices, universities, mobile carrier NAT — look the same. If two taps fit, Strybok
  refuses and nobody gets a link. If only one fits but a different person opens the app first,
  that person gets the link and its payload. This is why matching is opt-in per link: never
  allow invitations, links that grant access, or anything personal, and check
  `isProbabilisticMatch` in your app before doing anything that matters.
- **It often does not work at all:** not with iCloud Private Relay or a VPN (the browser and
  the app come from different addresses), not when the person changes network between the tap
  and the first launch, not for most iPads (their Safari says it is a Mac), and not after the
  time window.
- **It changes your privacy obligations.** Strybok processes your visitors' IP addresses for
  this (the address is not stored; the keyed hash is, for the window you chose). Your privacy
  policy and your App Store privacy labels must cover it, and you may need consent under
  GDPR/ePrivacy. This is not legal advice. See privacy.
- **Nobody knows how accurate it is.** Figures quoted by other vendors are unsourced. The link
  analytics show how many taps were kept, matched and refused as ambiguous; they cannot show
  how many matches were the wrong person.

If both options are on, the pasted link wins: redeeming it removes the stored guess, and the SDK
waits for the paste before it asks for a match.

Reference: [TN3155: Debugging universal links](https://developer.apple.com/documentation/technotes/tn3155-debugging-universal-links).
