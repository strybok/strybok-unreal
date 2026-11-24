# Keeping link creation from your app safe

Your app can create its own short links — the "Share" button — with a **client key**. This page
explains, in plain terms, what that key lets other people do, what Strybok does about it by
default, and how to switch on extra protection for iOS and Android.

> **Status:** the server side is built and covered by automated tests with synthetic data. It
> has **not** been verified on a physical device or against Apple's and Google's real services,
> and **no SDK supports the extra protection yet**. Read [the limits](#the-limits) before you
> rely on it.

## The risk: the client key is public

The client key (`pk_…`) is shipped inside your app. Anyone who downloads your app can take it
out — with free tools, in minutes. You should assume strangers have it. It is not a password and
it cannot be made secret.

With your client key, a stranger can:

- **Create links for the routes you allowed**, on your link hostname, with **any preview title,
  text and image** they choose. A link that looks like yours can carry their words.
- **Use up your daily allowance** of new links, so your real users cannot create links until the
  next day (UTC).

With your client key, a stranger **cannot**:

- create links for routes you did not allow;
- choose where a link sends people who do not have the app — that is always your own
  configuration;
- pick the link's address (the slug), read, change or disable your links, or see anything about
  your account.

Never put an API key (`lb_…`) in an app. That one is a real secret.

## What protects you by default

These apply to every application, without any set-up:

| Protection                           | What it does                                                                      |
| ------------------------------------ | --------------------------------------------------------------------------------- |
| Allowed routes                       | The key only creates links for the route patterns you listed. Keep them narrow.   |
| No destinations from the app         | Fallback and store URLs always come from your settings, never from the request.   |
| 20 requests a minute per IP address  | Slows down a single source.                                                       |
| 600 requests a minute per client key | Bounds the total across all users of your app.                                    |
| 1000 new links a day per application | A hard daily ceiling. Asking again for a link that already exists does not count. |
| "from app" marking                   | Links created with the client key are marked in the dashboard and the API.        |
| Replace key / switch off             | Both take effect on the next request.                                             |

What to do yourself:

- **Keep route patterns narrow.** `/product/*` is fine. `/**` allows every screen of your app.
- **Treat the payload as untrusted** when your app opens a link, exactly like any other input.
- **Know the cost of replacing the key:** it stops abuse at once, but released versions of your
  app cannot create links until users update to a version with the new key. There is no overlap
  period.

## Extra protection: prove it is really your app

You can ask Strybok to accept link requests only — or preferably — from a **genuine build of
your app on a genuine device**. The proof comes from the platform:

- **iOS:** Apple **App Attest**.
- **Android:** Google **Play Integrity**.

A copied client key is then much less useful, because a script on someone's laptop cannot
produce that proof.

In the dashboard, open your application → **Setup** → **Links from the app** → **Extra
protection (optional)** and choose:

| Setting       | Apps that prove they are genuine | Everything else                                                         |
| ------------- | -------------------------------- | ----------------------------------------------------------------------- |
| **Off**       | Normal limits                    | Normal limits                                                           |
| **Preferred** | Normal limits                    | Still works, but all of them together may create **50 new links a day** |
| **Required**  | Normal limits                    | **Refused**                                                             |

"Everything else" is not only attackers. It is also every real user whose device or app cannot
produce the proof — see [the limits](#the-limits). **Start with Preferred.** Move to Required
only when you have seen your released app pass.

You can set up one platform only. Then, with **Required**, apps on the other platform cannot
create links at all; with **Preferred**, they share the small allowance.

### Set up iOS (App Attest)

1. In the dashboard, make sure the application has your **Apple Team ID** and **bundle ID**
   (the same ones used for Universal Links). They must match the app exactly.
2. In Xcode, select your app target → **Signing & Capabilities** → **+ Capability** → **App
   Attest**. This adds the `com.apple.developer.devicecheck.appattest-environment` entitlement.
   Xcode sets it to `development` for builds you run from Xcode; App Store and TestFlight builds
   always use `production`.
3. Use an SDK version that supports attestation and switch its `attestation` option on. (None is
   released yet.)
4. While developing, tick **Accept development builds (iOS)** in the dashboard so builds run from
   Xcode are accepted. **Untick it before you rely on the protection**: a development build is
   easier to tamper with.
5. Test on a **real device**. App Attest does not work in the simulator.

### Set up Android (Play Integrity)

Google only lets the owner of an app read its integrity verdicts. So Strybok has to ask Google
on your behalf, and for that you give it a Google service-account key.

These steps follow Google's documentation; we have not yet walked through them with a real
Google project, and the names of menus in Google's consoles change from time to time.

1. In the dashboard, make sure the application has your **package name** and the **SHA-256
   fingerprints of your signing certificates**. If you use Play App Signing, that is the _app
   signing key_ certificate from Play Console → **Test and release** → **App integrity**, not
   your upload key.
2. In the [Google Cloud console](https://console.cloud.google.com/), choose (or create) a
   project and enable the **Google Play Integrity API** (APIs & Services → Library).
3. In the [Play Console](https://play.google.com/console/), open your app → **Test and
   release** → **App integrity** → **Play Integrity API** and **link that Cloud project**.
4. Back in the Cloud console: **IAM & Admin** → **Service Accounts** → **Create service
   account**. Give it a name such as `strybok-integrity`. Give it only what Google's Play
   Integrity documentation asks for to decode integrity tokens (at the time of writing the
   _Service Account User_ and _Service Usage Consumer_ roles). Do not make it an owner or editor
   of the project.
5. Open the new service account → **Keys** → **Add key** → **Create new key** → **JSON**. A
   `.json` file is downloaded.
6. In the Strybok dashboard, under **Android — Play Integrity**, choose **Upload Google key**
   and select that file. The dashboard then shows "Google key: configured" with the service
   account's address.
7. Delete the downloaded file from your computer.
8. Use an SDK version that supports attestation, switch its `attestation` option on and give it
   your Cloud **project number**. (None is released yet.)
9. Test with a build **installed from Google Play** (an internal testing track is enough).
   Builds installed from Android Studio are not "recognised by Play" and will be refused.

About that key:

- It is encrypted before it is stored, it is used only to ask Google about integrity tokens of
  this application, and **no screen or API ever shows it again** — it can only be replaced or
  removed.
- It is still a credential for your Google Cloud project held by someone else. Use a service
  account that exists only for this purpose, so you can delete its key in the Cloud console at
  any time without affecting anything else.
- To stop using it: **Remove Google key** in the dashboard, and delete the key in the Cloud
  console.

### What your app will see

An SDK with attestation support does all of this by itself the first time your app creates a
link, and about once an hour after that. Your code only meets two new errors (see the
[SDK contract](./sdk-contract.md#attestation-optional)):

- **`AttestationRequired`** — you chose Required and this device or build could not prove
  itself. On a simulator, a device without Google Play services or a sideloaded build that is
  expected. Tell the user sharing is not available on this device.
- **`AttestationFailed`** — a fresh proof was refused twice. This should not happen in a
  correctly set-up release build; check the dashboard set-up.

An SDK **without** attestation support never proves anything. With Required, its requests are
refused (`Unauthorized`); with Preferred, they use the small shared allowance.

## The limits

Please read all of them. Extra protection raises the cost of abuse; it does not make abuse
impossible, and it can lock out real users.

- **iOS: App Attest needs iOS 14 or later and a real device.** It does not work in the
  simulator and is not supported on iOS versions below 14. It can also be unavailable in some
  situations, such as app extensions and some Macs running iOS apps.
- **Android: Play Integrity needs Google Play services and an app installed from Google Play.**
  It does not work on Android devices without Google services — most devices sold in mainland
  China, Huawei devices without Google Mobile Services — or for sideloaded builds and builds
  from other app stores.
- **Google limits how often you can ask.** By default 10,000 integrity requests a day per app.
  If your app needs more, you have to request a higher tier from Google.
- **Android protection means giving Strybok a Google service-account key** with access to the
  Play Integrity API for your app. It is stored encrypted, but it is still a credential held by
  a third party.
- **With Required, any platform or device that cannot produce the proof cannot create links at
  all.** With Preferred, all of them share 50 new links a day.
- **What the proof means.** It shows that a request came from a genuine build of your app on a
  genuine device _at the moment the proof was made_. It does not stop a real user of your real
  app from creating links. A proof can also be lifted from a device the attacker controls and
  used elsewhere; to bound that, one proof is good for at most **30 new links** and **one
  hour**.
- **Not every SDK can do this.** Support is planned only for the native iOS SDK and the
  Android / Kotlin Multiplatform SDKs. The Flutter, React Native, Capacitor, Unity, .NET and
  Unreal SDKs have no attestation code yet, so apps built with them count as "cannot attest".
- **Not verified in the real world yet.** None of this has been tried on a physical device or
  against Apple's and Google's real services. Until it has, treat Required as experimental.

## If your key is being abused

1. Dashboard → **Links from the app** → untick **Allow the app to create links** to stop it
   immediately, or **Replace key** if you can ship an app update quickly.
2. In the links list, links created with the client key carry a "from app" badge. Disable the
   ones you did not expect.
3. Narrow the allowed routes.
4. Consider Extra protection, starting with Preferred.

More detail for the curious: decision 0015 (client
keys), decision 0016 (attestation) and the
threat model.
