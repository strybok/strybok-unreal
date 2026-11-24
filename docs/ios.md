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

| Symptom                                            | Likely cause                                                                                                                                            |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Link always opens Safari                           | Entitlement missing or wrong hostname; Team ID/bundle ID mismatch; Apple CDN still has the old file; the link was tapped in the same domain's web page. |
| Works in Notes, not in Instagram/Telegram/Facebook | In-app browsers often do not hand links to apps. Users see the fallback page with a manual button.                                                      |
| Works in a development build only                  | The App Store build is signed for a different Team ID or bundle ID than configured.                                                                     |

Changing the hostname in the entitlement requires shipping an app update.

Reference: [TN3155: Debugging universal links](https://developer.apple.com/documentation/technotes/tn3155-debugging-universal-links).
