# Android App Links setup

Goal: Android opens `https://<your managed hostname>/<slug>` directly in your app.

## 1. Tell Strybok about the app

Set on the application (`PATCH /v1/apps/:appId`):

- `androidPackageName` — the application ID, e.g. `com.example.app`
- `androidSha256Fingerprints` — SHA-256 fingerprints of **every** certificate that signs builds
  users install
- `androidStoreUrl` — your Google Play URL (used as the fallback)

Which fingerprints?

| Build               | Where to find it                                                                                                                                 |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| Google Play release | Play Console → _Setup → App signing_ → **App signing key certificate**. This is the one that matters in production. It is _not_ your upload key. |
| Local release build | `keytool -list -v -keystore <your.keystore> -alias <alias>`                                                                                      |
| Debug build         | `keytool -list -v -keystore ~/.android/debug.keystore -alias androiddebugkey -storepass android`                                                 |

Add the debug fingerprint too if you want to test App Links with debug builds.

Strybok then serves `https://<hostname>/.well-known/assetlinks.json`. Check it:

```sh
curl -i https://<hostname>/.well-known/assetlinks.json
```

Expected: `200`, `Content-Type: application/json`, no redirect, your package name and
fingerprints. An empty array `[]` means the package name or fingerprints are still missing.

## 2. Declare the link in the app

`android/app/src/main/AndroidManifest.xml`, inside the main `<activity>`:

```xml
<intent-filter android:autoVerify="true">
    <action android:name="android.intent.action.VIEW" />
    <category android:name="android.intent.category.DEFAULT" />
    <category android:name="android.intent.category.BROWSABLE" />
    <data android:scheme="https" android:host="<your managed hostname>" />
</intent-filter>

<!-- Flutter only: app_links handles links, disable the built-in handler -->
<meta-data android:name="flutter_deeplinking_enabled" android:value="false" />
```

Declare the whole host (no path filter); the slug is resolved at runtime. The exact snippet with
your hostname is returned by `GET /v1/apps/:appId/integration`.

## 3. Verify on a device

Android verifies the association when the app is installed, and caches the result.

```sh
# Verification state for your package ("verified" is what you want)
adb shell pm get-app-links <package>

# Force a re-check after changing fingerprints or the manifest, then look again
adb shell pm verify-app-links --re-verify <package>
adb shell pm get-app-links <package>

# Simulate tapping a link
adb shell am start -a android.intent.action.VIEW -c android.intent.category.BROWSABLE \
  -d "https://<hostname>/<slug>"
```

Strybok can only tell you that the file is served correctly. Whether Android verified it is
visible only on the device.

## Troubleshooting

| Symptom                                          | Likely cause                                                                                                                                                   |
| ------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| State is `legacy_failure` or `1024`              | Fingerprint mismatch (often the upload key instead of the Play App Signing key), wrong package name, or the file was not reachable when the app was installed. |
| Works for debug, not for the Play build          | The Play App Signing fingerprint is missing.                                                                                                                   |
| A chooser dialog appears                         | The link is not verified; without verification Android treats it as an ordinary web link.                                                                      |
| Changed fingerprints but nothing happens         | Verification is cached: re-verify or reinstall.                                                                                                                |
| Link opens a page from Instagram/TikTok/Facebook | See [Links opened from Instagram, TikTok and other apps](#links-opened-from-instagram-tiktok-and-other-apps).                                                  |

Changing the hostname in the manifest requires shipping an app update.

### Links opened from Instagram, TikTok and other apps

Instagram, Facebook, Messenger, TikTok, LinkedIn, Snapchat, LINE, WeChat and similar apps open
links in their own built-in browser, where App Links often do not fire even when your app is
installed.

When Strybok recognises such a browser (best-effort, from the User-Agent) it shows a page instead
of redirecting to Google Play, with:

- **Open in the app** — an `intent:` link for the same `https` link, addressed to your package
  name, with the usual destination as the fallback when the app is missing. It needs the Android
  package name in the application settings. Chrome and Chrome-based browsers follow it; a
  built-in browser based on a plain WebView only does if its app implemented that, and
  Instagram's and Facebook's are reported not to.
- the Google Play (or fallback) button;
- a line telling the visitor to choose "Open in browser" from the menu if the button does
  nothing.

None of this has been verified inside the real apps or on a device. Details:
decision 0017.

## Links tapped before the app is installed (deferred links)

Optional and off by default. The server side is deployed, and the app side is implemented in the
SDK versions released on 2026-11-25: the Android SDK and Kotlin Multiplatform 0.4.0 ship the
store referrer sources (you add the store's library); Flutter, JavaScript, React Native,
Capacitor, Unity, .NET and Unreal 0.3.0 implement the flow but your app has to hand them the
referrer string (see
[which SDKs implement what](./sdk-contract.md#which-sdks-implement-deferred-links); the exact
behaviour is in the [SDK contract](./sdk-contract.md#deferred-links-optional-android)).
**Nothing below has been verified on a device or against a real store** — only by unit tests —
and only the Google Play row is written against the store's own documentation. Design:
decision 0019.

**What it does.** A person without your app taps a link and is sent to your store page. Strybok
adds a random, single-use code to that store URL. The store keeps it through the installation
and hands it to your app, the SDK exchanges it for the link, and your app opens the right
screen about a second after its first start.

**To enable it**

1. In the dashboard, under **When a link opens in a browser**, switch on **Restore the link
   after install (Android only)** — or `PATCH /v1/apps/{appId}` with
   `{ "deferredLinksEnabled": true }` (`apps:write`). The Android package name must be set.
   Do this when an app version that redeems the code is in the store; before that, codes are
   handed out for nothing.
2. In the app, with an SDK version that has the option: switch the SDK's `deferredLinks` option
   on, give it the referrer sources for the stores you publish in, and add the store's library
   (SDKs without native code of their own need the referrer string from your app instead — see
   that SDK's guide):

   | Store                | Your store URL must look like                                                                   | Add to the app                                                                                    | Standing                         |
   | -------------------- | ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- | -------------------------------- |
   | Google Play          | `https://play.google.com/store/apps/details?id=<package>`                                       | `com.android.installreferrer:installreferrer:2.2`                                                 | Per Google's documentation       |
   | Huawei AppGallery    | `https://appgallery.cloud.huawei.com/appDetail?pkgName=<package>&detailType=0&callType=AGDLINK` | No library (AppGallery's content provider); possibly a manifest `<queries>` entry — unverified    | **Unverified**                   |
   | Samsung Galaxy Store | `https://apps.samsung.com/appquery/appDetail.as?appId=<package>`                                | `store.galaxy.samsung.installreferrer:samsung_galaxystore_install_referrer:4.0.0` (Maven Central) | **Unverified**, third-party info |

3. Decide whether your users must consent before the app reads the store's install referrer
   (it is information stored on the device; Huawei requires consent for its referrer), and
   switch `deferredLinks` on only after that. Mention the feature in your privacy policy and in
   the stores' data-safety forms: a random code, made when the link was tapped, is read once
   and sent to Strybok; nothing about the device is.

**Where the code is added.** Only for Android visitors, and only when the destination Strybok
would send them to anyway is one of the three URL shapes above for **your** package: the
application's Google Play URL, a link's Android fallback, or a country rule (for example
`CN → AppGallery`). The application's "Google Play URL" field accepts only `play.google.com`,
so AppGallery and Galaxy Store pages go into country rules or a link's Android fallback. Other
store pages are left alone — in particular `https://galaxystore.samsung.com/detail/<package>`
and `https://appgallery.huawei.com/app/C…`, which do not pass a referrer on. `market://`,
`samsungapps://` and `hiapplink://` links cannot be destinations at all.

If your store URL already has a `referrer` made of `key=value` pairs (for example
`referrer=utm_source%3Dnewsletter`), it is kept and our key is put in front of it. If it has any
other `referrer`, Strybok leaves the URL exactly as you stored it and deferred links do not
work for that URL. With "pass campaign parameters to the destination" on, the tap's `utm_*`
values are added inside the referrer as well.

**Limits — it does not work when**

- the person installs any other way than from the store page the link opened: after searching
  the store, from the store's **website** (which is where many in-app browsers end up), from
  another store, from an APK file, by restoring a backup, or on a device where the app was
  pre-installed;
- more than **24 hours** pass between the tap and the first start of the app;
- the device has no Google Play Store, AppGallery or Galaxy Store app, or one too old for the
  install referrer (Play Store 8.3.73, AppGallery 10.5.0.300, Galaxy Store 4.5.20.1);
- the app was already installed (then the link opens the app directly, as always);
- the code was already used: it works once, and a second device or a cleared app gets nothing;
- you switched the feature off, or disabled the link, in the meantime.

In all of these the person simply lands on your app's first screen, as today. The link opens
after the app has started, not as its launch link; `getInitialLink` does not return it. If the
person taps a normal link of your app before that, the normal link wins. On the landing page
(in-app browsers, "always show the landing page") the store button and the "Open in the app"
fallback carry the code too.

**Analytics.** "Deferred links offered" counts codes handed out (once per store redirect and
once per landing page shown), "Deferred opens" counts codes exchanged by an installed app. Both
are counted by the server; neither is a number of installs.

**iOS** has no install referrer. What exists there instead — two opt-in options with real
costs — is in the [iOS guide](./ios.md#links-tapped-before-the-app-is-installed).

Reference: [Verify App Links](https://developer.android.com/training/app-links/verify-applinks),
[Play Install Referrer Library](https://developer.android.com/google/play/installreferrer/library).
