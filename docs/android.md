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

| Symptom                                  | Likely cause                                                                                                                                                   |
| ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| State is `legacy_failure` or `1024`      | Fingerprint mismatch (often the upload key instead of the Play App Signing key), wrong package name, or the file was not reachable when the app was installed. |
| Works for debug, not for the Play build  | The Play App Signing fingerprint is missing.                                                                                                                   |
| A chooser dialog appears                 | The link is not verified; without verification Android treats it as an ordinary web link.                                                                      |
| Changed fingerprints but nothing happens | Verification is cached: re-verify or reinstall.                                                                                                                |
| Link opens Chrome from another app       | Some in-app browsers never hand links to apps. Users see the fallback page.                                                                                    |

Changing the hostname in the manifest requires shipping an app update.

Reference: [Verify App Links](https://developer.android.com/training/app-links/verify-applinks).
