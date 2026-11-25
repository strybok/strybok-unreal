# SDK contract

Every Strybok SDK behaves the same way. The Flutter SDK
([github.com/strybok/strybok-flutter](https://github.com/strybok/strybok-flutter)) is the
reference implementation; its test suite is the reference test list. This page is the
language-neutral version of it.

Deferred links (restoring a link after a store install) are specified in
[Deferred links (Android)](#deferred-links-optional-android) and
[Deferred links on iOS](#deferred-links-on-ios-optional): optional, off by default, and very
different on the two platforms. Which SDK versions implement them is listed in
[Which SDKs implement deferred links](#which-sdks-implement-deferred-links). All of them are
unreleased at the time of writing and **none has run on a device, against a real store, with a
real pasteboard or against the production API**. There is no automatic deferred deep linking on
iOS. An SDK version without the `deferredLinks` option restores nothing after installation.

## Configuration

| Option                       | Default  | Meaning                                                                                                                                                                                                                                                                                                                    |
| ---------------------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `apiBaseUrl`                 | required | Base URL of the API, e.g. `https://api.strybok.com`. A path prefix is preserved.                                                                                                                                                                                                                                           |
| `allowedHosts`               | required | Link hostnames of this app, compared lower-case. Other hosts are ignored.                                                                                                                                                                                                                                                  |
| `timeout`                    | 5 s      | Time allowed for one resolve attempt.                                                                                                                                                                                                                                                                                      |
| `maxRetries`                 | 2        | Extra attempts after a timeout, network error or 5xx.                                                                                                                                                                                                                                                                      |
| `retryDelay`                 | 400 ms   | Pause before the first retry; doubles for each further retry.                                                                                                                                                                                                                                                              |
| `dedupeWindow`               | 2 s      | The identical URL received again within this window is dropped.                                                                                                                                                                                                                                                            |
| `reportAppOpens`             | `true`   | Send a best-effort `app_open` event after a link was resolved.                                                                                                                                                                                                                                                             |
| `onError`                    | none     | Receives errors for our links that could not be resolved.                                                                                                                                                                                                                                                                  |
| `clientKey`                  | none     | Publishable key `pk_…` of the application. Only needed for `createLink`.                                                                                                                                                                                                                                                   |
| `attestation`                | `false`  | Prove to the server that this is the genuine app before creating links. Only SDKs that support it have this option (Android also needs `cloudProjectNumber`), see [Attestation](#attestation-optional).                                                                                                                    |
| `deferredLinks`              | `false`  | Restore a link that was tapped before the app was installed. Android: ask the store for its install referrer on the first launch, see [Deferred links](#deferred-links-optional-android). iOS: only the two flows of [Deferred links on iOS](#deferred-links-on-ios-optional). Only SDKs that support it have this option. |
| `allowProbabilisticMatching` | `false`  | iOS only, and only together with `deferredLinks`: let the server guess the link from the network address, see [Probabilistic matching](#flow-b-probabilistic-matching). Forbidden by Apple's rules; the app developer opts in here in code, separately from the owner's setting.                                           |

The source of incoming URLs (`LinkSource`: a launch URL plus a stream of later URLs) and the HTTP
transport are replaceable, so the SDK can be tested without a device or a network.

## Which URLs are ours

A URL is a Strybok link of this app only when **all** of these hold:

- scheme is `https`;
- host, lower-cased, is in `allowedHosts`;
- no user info (`user:pass@`);
- no port, or port 443;
- path matches `^/[A-Za-z0-9_-]{1,64}$` (exactly one slug segment, no trailing slash).

Query and fragment are allowed and do not decide whether a URL is ours. Anything else is dropped
silently by the automatic flow (no error, no network call) and rejected with `UnsupportedLink` by
`resolve`.

## Resolving

`GET {apiBaseUrl}/v1/public/resolve?url={lookup}` with `Accept: application/json`, where `lookup`
is `https://{lower-cased host}{path}` followed by the **campaign query** described below. The rest
of the query and the fragment are **never** sent, they may hold private data.

### Campaign query

_Added on 2026-11-24 (decision 0018). SDK versions
released before that send no query at all; see "Older SDK versions" below._

From the query string of the tapped URL the SDK keeps **only** these keys, compared exactly
(lower case): `utm_source`, `utm_medium`, `utm_campaign`, `utm_term`, `utm_content`.

1. Split the raw query on `&`, each part on its first `=`; percent-decode key and value as UTF-8,
   treating `+` as a space (a standard query parser such as `URLSearchParams`, `URLComponents`
   or `Uri.getQueryParameter` does this).
2. For each of the five keys take its **first** occurrence. Drop it unless the decoded value is
   1–64 characters long and consists only of `A–Z a–z 0–9 . _ ~ -`.
3. If nothing is left, `lookup` has no `?` at all — exactly what earlier versions sent.
4. Otherwise append `?` and the kept pairs as `key=value` joined with `&`, in the fixed order
   `utm_source`, `utm_medium`, `utm_campaign`, `utm_term`, `utm_content`. The allowed characters
   never need encoding inside `lookup`. (`lookup` as a whole is then percent-encoded as the value
   of `url`, as before.)

Example: tapped `https://MyApp.strybok.link/invite777?utm_source=instagram&fbclid=abc&utm_medium=two%20words&utm_source=x#top`
→ `lookup` = `https://myapp.strybok.link/invite777?utm_source=instagram`.

The server applies the same rule again and ignores everything else, so an SDK that passed more
would not change the answer — but it must not, for privacy. Dropping a value is never an error.

| Outcome                                                                                   | Result            | Retried |
| ----------------------------------------------------------------------------------------- | ----------------- | ------- |
| 200 with `linkId`, `appId`, `routePath` strings; `routePath` starts with `/` but not `//` | resolved link     | —       |
| 200 with anything else (not JSON, missing fields, bad `routePath`)                        | `Server` (200)    | no      |
| 404                                                                                       | `LinkNotFound`    | no      |
| other 4xx                                                                                 | `Server` (status) | no      |
| 5xx                                                                                       | `Server` (status) | yes     |
| transport failure                                                                         | `Network`         | yes     |
| no answer within `timeout`                                                                | `Timeout`         | yes     |

After `maxRetries` extra attempts the last error is reported. `payload` is the JSON object from the
response, or an empty object when it is missing or not an object.

The response may carry `campaign`: an object with any of the string fields `source`, `medium`,
`campaign`, `term`, `content`. It holds the link's default campaign values, overridden per key by
the campaign query that was sent. The field is **absent** when there is no value at all. The SDK
keeps the string fields with those five names and ignores everything else; a missing `campaign`,
one that is not an object, or a field that is not a string, is never an error.

A resolved link has: the original `uri` (as received, with query), `linkId`, `appId`, `routePath`,
`payload`, and `campaign` — an object or record with the optional string properties `source`,
`medium`, `campaign`, `term`, `content`, **never null**: it is empty when the server sent none.
`routePath`, `payload` and `campaign` are untrusted data chosen by whoever created or shared the
link: apps map them onto routes they already know, and treat campaign values as labels only.

`dedupeWindow` keeps comparing the full URL string as received, so the same slug tapped with
different campaign parameters is two links.

**Older SDK versions.** A version that sends no campaign query keeps working unchanged: the
server answers with the link's default values in `campaign` (or without the field), which such a
version ignores. Its users are simply not attributed to the campaign of the URL they tapped, and
its `app_open` events are counted under the link's defaults (below).

## Delivery

- `initialize`/`start` subscribes to the URL stream **first**, then reads the launch URL, and
  completes once the launch link has been resolved or has failed. Calling it again is harmless.
- Every resolved link is delivered **exactly once**. Links resolved while nobody listens (the
  cold-start link, typically) are queued and delivered as soon as a listener attaches.
- `getInitialLink` returns the launch link at most once and removes it from the queue, so using
  both APIs never navigates twice. It returns nothing if the listener already received the link.
- The same URL string arriving again within `dedupeWindow` is a duplicate delivery of one tap
  (platforms often report the launch URL twice) and is dropped. The same link tapped again later
  is delivered again.
- A failure goes to `onError`; later links keep working.
- After `dispose`/`stop` nothing is delivered, including a link that was still resolving.

## `app_open` event

After a link is delivered (or queued), when `reportAppOpens` is on:

`POST {apiBaseUrl}/v1/public/events`, `Content-Type: application/json`

```json
{
  "eventId": "<random UUID v4>",
  "linkId": "<linkId>",
  "type": "app_open",
  "platform": "ios | android | unknown",
  "sdkVersion": "<SDK version, at most 32 characters>",
  "campaign": { "source": "instagram", "medium": "social" }
}
```

`campaign` is the resolved link's `campaign`, **exactly as the server returned it** (only the five
string fields). It is **omitted** when it is empty. The SDK does not build it from the URL itself:
what resolve returned is what is reported. The server validates it like every other input (422
for an unknown field or a value outside the allowed characters, which cannot happen with values
that came from resolve), applies it on top of the link's defaults, and counts the open per
source, medium and campaign. An event without `campaign` is counted under the link's defaults.

No other fields are allowed. The call is never awaited by delivery, and any failure is swallowed:
analytics must never delay or break navigation.

## Creating links

`createLink(routePath, payload?, title?, description?, imageUrl?)` asks the service for a short
link to a screen of this app — the "Share" button. It returns the created link or fails with one
of the typed errors below. The server side is described in
decision 0015.

It needs `clientKey`, the application's publishable key from the dashboard ("Links from the
app"). The key is safe to ship inside the app: it can only create links for the route patterns
the owner listed. A secret API key (`lb_…`) must never be put in an app and is never accepted
here.

**When it runs.** Only when the app calls it. `initialize`/`start`, resolving and delivery never
call it, wait for it or depend on it, so it is never on the app's start path. It does not need
`allowedHosts` and works before or without `initialize`/`start`.

**Before any network call**

| Check                                                              | Failure        |
| ------------------------------------------------------------------ | -------------- |
| `clientKey` is configured and not empty                            | `Unauthorized` |
| `routePath` starts with `/`, does not start with `//`, ≤ 512 chars | `Validation`   |
| `payload`, when given, is a JSON object (not an array or a scalar) | `Validation`   |

Everything else is validated by the server; SDKs do not copy the server's rules.

**Request**

`POST {apiBaseUrl}/v1/public/links` with headers `Content-Type: application/json`,
`Accept: application/json` and `X-Strybok-Key: {clientKey}`. No `Authorization` header. SDKs that
support [attestation](#attestation-optional) and have it switched on add
`X-Strybok-Attestation: {token}`; all other SDKs never send that header.

```json
{
  "routePath": "/product/42",
  "payload": { "productId": 42 },
  "title": "Red shoes",
  "description": "Look at these",
  "imageUrl": "https://cdn.example.com/shoes.png"
}
```

Only `routePath` is required. Arguments that were not given are **omitted** from the body (the
server treats `null` like an omitted field, but SDKs do not send it). No other fields are
allowed: the server rejects a slug, fallback URLs, an expiry or anything else with 422. Limits
enforced by the server: `payload` at most 4096 bytes of JSON, `title` 120 and `description` 300
characters, `imageUrl` a public HTTPS URL, body 16 KB.

**Response**

| Outcome                                                                              | Result            | Retried |
| ------------------------------------------------------------------------------------ | ----------------- | ------- |
| 201 or 200 with `linkId`, `slug`, `routePath` strings and `url` an `https://` string | created link      | —       |
| 201 or 200 with anything else (not JSON, missing fields, `url` not HTTPS)            | `Server` (status) | no      |
| 401 (see the note on `error.code` below)                                             | `Unauthorized`    | no      |
| 403                                                                                  | `RouteNotAllowed` | no      |
| 429                                                                                  | `RateLimited`     | no      |
| 400, 413, 422                                                                        | `Validation`      | no      |
| other 4xx                                                                            | `Server` (status) | no      |
| 5xx                                                                                  | `Server` (status) | yes     |
| transport failure                                                                    | `Network`         | yes     |
| no answer within `timeout`                                                           | `Timeout`         | yes     |

200 means an identical link already existed and was returned; 201 means a new one was made. Both
are success and SDKs do not need to tell them apart.

**401 and `error.code`.** The owner can ask for attestation (see
[Attestation](#attestation-optional)). The server then answers 401 with `error.code`
`ATTESTATION_REQUIRED` or `ATTESTATION_INVALID` instead of `UNAUTHENTICATED`. An SDK **without**
attestation support does not need to look at the code: it reports every 401 as `Unauthorized`,
which stays true ("the app or the dashboard settings need a change"). An SDK **with** attestation
support reads `error.code` on a 401 and maps it as described in that section.

The created link has `url` (what the user shares), `slug`, `routePath` and `linkId`. The SDK does
not check `url` against `allowedHosts`.

**Timeout and retries** use the same `timeout`, `maxRetries` and `retryDelay` as resolving: up to
`maxRetries` extra attempts after a timeout, transport failure or 5xx, waiting `retryDelay` before
the first retry and doubling it each time; then the last error is reported. Retrying is safe
because creation is idempotent: the same application and the same content (`routePath`,
`payload` regardless of key order, `title`, `description`, `imageUrl`) always return the same
active link, so a retry after a lost response cannot create a second one. The retried request
must have exactly the same body. 4xx answers, including 429, are never retried automatically.

**Errors of `createLink`**

| Kind              | When                                                                                                                                                         | Carries                                                         |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------- |
| `Unauthorized`    | No `clientKey` configured (no network call), or 401: the key is wrong or was replaced, or the owner switched the feature off. The server does not say which. | —                                                               |
| `RouteNotAllowed` | 403: `routePath` is not covered by the application's route patterns.                                                                                         | the `routePath`                                                 |
| `RateLimited`     | 429: too many requests from this device's address or for this app, or the app's daily limit of new links is reached.                                         | `retryAfter` seconds from the `Retry-After` header, if any      |
| `Validation`      | Rejected before sending, or 400/413/422 from the server.                                                                                                     | the server's `error.message` and `error.details.issues`, if any |
| `Network`         | The request could not be completed, after all retries.                                                                                                       | —                                                               |
| `Timeout`         | No answer in time, after all retries.                                                                                                                        | —                                                               |
| `Server`          | 5xx after all retries, any other status, or a success answer that cannot be read.                                                                            | the status code                                                 |

`Network`, `Timeout` and `Server` are the same kinds as for resolving. Errors of `createLink` are
returned to the caller (thrown, or delivered through the call's own result), never through
`onError`, which is for incoming links only. They never contain the `clientKey` or the payload.

What an app should do: `RateLimited`, `Network`, `Timeout` and `Server` are temporary — let the
user try again later (after `retryAfter` when present). `Unauthorized`, `RouteNotAllowed` and
`Validation` will not go away by retrying; they mean the app or the dashboard settings need a
change.

## Attestation (optional)

The owner of an application can ask that links are only created — or preferably created — by a
genuine build of the app on a genuine device. The proof is Apple **App Attest** on iOS and Google
**Play Integrity** on Android. The server side is described in
decision 0016; the limits an app developer should know
are in [link-creation-security.md](./link-creation-security.md).

**Which SDKs.** Attestation needs native platform code. It is implemented in the native iOS SDK and
the Android / Kotlin Multiplatform SDKs. **An SDK without support simply never sends the
`X-Strybok-Attestation` header** and changes nothing else; the server then treats the app
according to the owner's setting (served, served from a small shared allowance, or refused with
401). Everything below applies only to an SDK that implements attestation.

**Configuration.** One option, `attestation` (boolean, default `false`). When it is `false` the
SDK behaves exactly like an SDK without support. It needs `clientKey`.

**When it runs.** Lazily, inside `createLink` only. Never from `initialize`/`start`, resolving or
delivery, never in the background, never ahead of time. An app that does not call `createLink`
makes no attestation request and generates no key.

### Flow

All requests go to `{apiBaseUrl}` with `Content-Type: application/json`,
`Accept: application/json` and `X-Strybok-Key: {clientKey}`, and use `timeout` like every other
request. `createLink` with `attestation` on does this:

1. If a cached session token exists and is not about to expire, go to step 4.
2. `POST /v1/public/attest/challenge` (no body) → `{ "challenge": "…", "expiresAt": "…" }`.
   `challenge` is 43 characters of base64url. Treat it as an opaque string and use it once, right
   away (it is valid for 5 minutes and for one proof attempt).
3. Turn the challenge into a platform proof and exchange it for a session token (below). The
   answer is always `{ "token": "…", "expiresAt": "<ISO-8601 UTC>" }`. Cache it **in memory
   only**.
4. Send `POST /v1/public/links` as usual, plus `X-Strybok-Attestation: {token}`.

**Token cache.** One token per SDK instance, in memory, never written to disk, logs or errors.
Stop using it 60 seconds before `expiresAt`. Concurrent `createLink` calls share one attestation
in flight; they do not start one each. The token is opaque: do not parse it.

### iOS (App Attest)

Uses `DCAppAttestService.shared`. `clientDataHash` is always
**SHA-256 of the UTF-8 bytes of the `challenge` string** (32 bytes).

- **Unavailable** when `DCAppAttestService.shared.isSupported` is `false` (simulator, iOS below
  14, some extensions and Macs), or any App Attest call fails with something other than a server
  answer. See "When attestation is unavailable".
- **Key id.** The SDK keeps one App Attest key id per `clientKey`, persisted in the Keychain
  (preferred; `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`) or in app storage. It is not a
  secret, but it must survive app restarts.
- **First time (no stored key id):** `generateKey()` → `keyId`;
  `attestKey(keyId, clientDataHash:)` → attestation object. Then

  ```http
  POST /v1/public/attest/ios/register
  { "keyId": "<as generateKey returned it>", "attestation": "<base64>", "challenge": "<challenge>" }
  ```

  `keyId` is sent unchanged (standard base64, 44 characters). `attestation` is the attestation
  object in **standard base64 with padding**. Store the key id only after the server answered 200.

- **Afterwards (stored key id):** `generateAssertion(keyId, clientDataHash:)` → assertion object.
  Then

  ```http
  POST /v1/public/attest/ios/assert
  { "keyId": "<stored key id>", "assertion": "<base64>", "challenge": "<challenge>" }
  ```

- **Starting over.** Delete the stored key id and run the first-time path, with a **new
  challenge**, once, when `/ios/assert` answers `403 ATTESTATION_FAILED` with `details.reason`
  `unknown_key`, or when `generateAssertion` fails with `DCError.invalidKey` (the key is gone,
  for example after a reinstall or a device restore).
- `attestKey` contacts Apple and can fail with `DCError.serverUnavailable`: that is "unavailable"
  for this call; do not discard the key id, try again on the next `createLink`.

### Android (Play Integrity)

Uses the Play Integrity **standard** API (`StandardIntegrityManager`). The app needs the Play
Integrity library and its Google Cloud project number in the SDK configuration
(`cloudProjectNumber`, required when `attestation` is on).

- `requestHash` is the **base64url encoding without padding of the SHA-256 of the UTF-8 bytes of
  the `challenge` string** — 43 characters. Example: challenge `abc` →
  `ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0`.
- Prepare the token provider (`prepareIntegrityToken`) on first use and keep it for the life of
  the SDK instance; request a token with that `requestHash`. If the provider reports it has
  expired (`INTEGRITY_TOKEN_PROVIDER_INVALID`), prepare it again once.
- Then

  ```http
  POST /v1/public/attest/android
  { "integrityToken": "<token as returned>", "challenge": "<challenge>" }
  ```

- **Unavailable** when Google Play services or the Play Store are missing or too old, the request
  fails (`API_NOT_AVAILABLE`, `PLAY_STORE_NOT_FOUND`, `PLAY_SERVICES_NOT_FOUND`, network errors
  and so on), or no `cloudProjectNumber` is configured.

### Answers of the attestation endpoints

| Outcome                                                                                   | What the SDK does                                                         |
| ----------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| 200 with `token` (string) and `expiresAt`                                                 | Cache the token, continue with `createLink`                               |
| 401 `UNAUTHENTICATED`                                                                     | `createLink` fails with `Unauthorized` (the key is wrong or switched off) |
| 403 `ATTESTATION_FAILED`, reason `unknown_key` (iOS, from `/ios/assert`)                  | Start over once, as described above                                       |
| 403 `ATTESTATION_FAILED`, any other reason                                                | Attestation is **unavailable** for this call                              |
| 409 `ATTESTATION_NOT_CONFIGURED`                                                          | Unavailable (the owner has not set this platform up)                      |
| 429                                                                                       | Unavailable for this call; do not retry the attestation now               |
| 503 `ATTESTATION_NOT_CONFIGURED` / `ATTESTATION_UNAVAILABLE`, other 5xx, timeout, network | Unavailable for this call                                                 |
| 200 with anything else, other statuses                                                    | Unavailable for this call                                                 |

Attestation requests are **not retried** on their own (a challenge is spent by every attempt).
The reason codes of `ATTESTATION_FAILED` are: `challenge`, `malformed`, `chain`, `nonce`,
`key_id`, `app_id`, `counter`, `environment`, `key_exists`, `unknown_key`, `signature` (iOS) and
`challenge`, `malformed`, `package`, `request_hash`, `stale`, `app_not_recognized`, `certificate`,
`device_integrity` (Android). SDKs may log the reason in debug builds; they must not log the
proof, the challenge or the token.

### When attestation is unavailable

The SDK **sends the `createLink` request without the `X-Strybok-Attestation` header and reports
the server's answer.** It does not fail on its own and does not guess the owner's setting: the
server serves the request (mode `off` or `preferred`) or refuses it with
`401 ATTESTATION_REQUIRED`. After an unavailable attempt the SDK does not try to attest again for
60 seconds, so a device that cannot attest does not pay for a failed attempt on every share.

### `createLink` answers that concern attestation

| Answer                                          | Result                                                                                                                                                                                                                                                      |
| ----------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 401 `ATTESTATION_INVALID`                       | Discard the cached token, attest again (new challenge) and send the same `createLink` request again — **once** per `createLink` call. If the second answer is again `ATTESTATION_INVALID`, or attestation is now unavailable, fail with `AttestationFailed` |
| 401 `ATTESTATION_REQUIRED`                      | `AttestationRequired`. Not retried. (Only happens when the request went out without a token.)                                                                                                                                                               |
| 429 with `error.details.scope` = `"unattested"` | `RateLimited`, as for every 429                                                                                                                                                                                                                             |
| 503 `ATTESTATION_NOT_CONFIGURED`                | `Server` (503); retried like any 5xx                                                                                                                                                                                                                        |

The re-attestation after `ATTESTATION_INVALID` is separate from, and does not count against,
`maxRetries`. `details.reason` is `expired`, `invalid` or `exhausted` (a token is good for a
limited number of new links); the SDK treats all three the same way.

New error kinds of `createLink`, for SDKs with attestation support only:

| Kind                  | When                                                                                                                                           | Carries                        |
| --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| `AttestationRequired` | 401 `ATTESTATION_REQUIRED`: the owner only accepts attested apps and this device or build could not attest (or `attestation` is switched off). | —                              |
| `AttestationFailed`   | 401 `ATTESTATION_INVALID` twice in a row: a fresh token was refused as well.                                                                   | `error.details.reason`, if any |

Neither goes away by retrying. `AttestationRequired` on a device that cannot attest (simulator,
no Google Play services, sideloaded build) is expected; apps should tell the user that sharing
is not available on this device rather than show an error. SDKs without attestation support
report both cases as `Unauthorized`.

### What attestation sends

In addition to what `createLink` sends: the App Attest key id, attestation and assertion objects
(iOS) or the Play Integrity token (Android), and the server's challenge. No device identifier is
read or sent by the SDK; the App Attest key id is random per installation and the Play Integrity
token is readable only by Google.

## Deferred links (optional, Android)

_Added on 2026-11-24 (decision 0019). Implemented,
covered by unit tests only, by Android (Kotlin) and Kotlin Multiplatform 0.4.0 with the shipped
store sources, and by Flutter, JavaScript, React Native, Capacitor, Unity, .NET and Unreal 0.3.0
with sources the app supplies — see
[Which SDKs implement deferred links](#which-sdks-implement-deferred-links). SDK versions
without the `deferredLinks` option are described under "Older SDK versions" at the end of this
section._

A person taps a link, does not have the app, installs it from the store page the link sent them
to, and opens it: the SDK delivers that link on the first launch. This works only

- on **Android**;
- when the owner switched "restore the link after install" on for the application;
- when the installation went through the **store app** of Google Play, Huawei AppGallery or
  Samsung Galaxy Store, from the page the link opened (not after a search, not from the store's
  website, not from an APK file);
- within **24 hours** of the tap;
- once.

Google Play is specified from Google's documentation. **AppGallery and Galaxy Store are
unverified**: their parts below come from Huawei's published pages and from Samsung's published
library, but how those stores treat our value was never tried, and nothing in this section has
run on a device. iOS has no install referrer; what exists there instead is in
[Deferred links on iOS](#deferred-links-on-ios-optional).

### Configuration

| Option                   | Default  | Meaning                                                                                                                                                                          |
| ------------------------ | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `deferredLinks`          | `false`  | Run the check below. With `false` the SDK does nothing described in this section: no source is asked, no request is made, nothing is stored.                                     |
| `installReferrerSources` | empty    | Ordered list of referrer sources (below). Empty means the check ends at once with status `unavailable`.                                                                          |
| `androidPackageName`     | platform | The app's own application ID. SDKs with native Android code read it from the platform and have no such option; SDKs without native code (pure Dart, pure JavaScript) require it. |

**Referrer source** (`InstallReferrerSource`): one operation, called off the main thread, that
answers with exactly one of

- **referrer** — the raw install referrer string of the store the app was installed from,
  exactly as the store returned it (it may be empty);
- **unavailable for now** — the store could not be asked; a later launch may succeed;
- **unavailable** — this store is not on the device, is too old, or its library is not in the
  app. Asking again is pointless.

A source must not throw, must give its answer within 10 seconds (the SDK treats a longer wait
as "unavailable for now" and abandons the call), and must release whatever connection it
opened. The source is replaceable, like `LinkSource` and the transport, so everything here is
testable without a device.

### State

The SDK keeps two values in its own private storage, per installation: `attempts` (integer,
starts at 0) and `done` (boolean). They are removed with the app's data. Nothing else about the
check is stored — not the referrer, not the click id.

### When it runs

At most once per process, **after** `initialize`/`start` has completed everything it does
today. `initialize`/`start`, the launch link, `getInitialLink` and `createLink` never wait for
the check and never depend on its result. A slow or hanging source delays nothing but the
deferred link.

SDKs with native Android access also skip the check — status `notFirstLaunch`, `done` stored —
when the platform reports that the app was updated since it was installed
(`PackageInfo.firstInstallTime != lastUpdateTime`), so a person who merely updates to an app
version with this feature is not asked about an old referrer. SDKs without that access rely on
the server: an id older than 24 hours is refused.

### Flow

1. `deferredLinks` is off → status `disabled`. Stop.
2. `done` is stored → status `notFirstLaunch`. Stop.
3. `attempts` is 3 or more → store `done`; status `failed`. Stop.
4. **Store `attempts + 1` now**, before anything else happens. (Why: see
   [Crashes](#crashes-and-the-once-only-guarantee).)
5. Ask the sources in order, one after another. For each answer:
   - a referrer in which a click id is found (next paragraph) → use that id; ask no further
     source;
   - anything else → remember the kind of answer and ask the next source.

   If no source produced an id:
   - at least one said "unavailable for now" → status `pending`; stop **without** storing
     `done`. The next launch runs the flow again, until step 3 ends it;
   - otherwise store `done`; status `noReferrer` if any source returned a string, else
     `unavailable`. Stop.

6. If a link of this app has been accepted (it passed ["Which URLs are ours"](#which-urls-are-ours))
   since the process started → store `done`; status `superseded`. **No request is made.**
7. `POST {apiBaseUrl}/v1/public/deferred-links/redeem` with `Content-Type: application/json`,
   `Accept: application/json` and

   ```json
   {
     "platform": "android",
     "clickId": "dc_…",
     "packageName": "<application ID>",
     "sdkVersion": "<SDK version>"
   }
   ```

   (`platform` may be left out by an Android SDK; the server then assumes `android`.)

   No other fields: the request carries nothing about the device, and none of the rest of the
   referrer. `timeout`, `maxRetries` and `retryDelay` apply as for resolving; a retry sends the
   identical body (the server answers a repeat within two minutes with the same link).

**Finding the click id in a referrer.** Split the string on `&`, each part on its first `=`.
Take the **first** part whose key is exactly `strybok_click`; its value must match
`^dc_[0-9A-Za-z]{32}$` as it stands (those characters are never encoded). If no part has that
key and the string contains `strybok_click%3D` (in any letter case), percent-decode the whole
string once and try again — a store may return the value still encoded. Everything else in the
referrer — campaign keys, other vendors' keys, timestamps a library offers — is **ignored**: not
parsed further, not stored, not sent, not logged. A missing key or a value that does not match
is "no id", never an error.

| Outcome of the request                                                                                                                                                      | Result                                                                                       |
| --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| 200 with `linkId`, `appId`, `routePath` valid as for resolving, `deferred` = `true`, and `url` a link of this app (it passes ["Which URLs are ours"](#which-urls-are-ours)) | Store `done`, then deliver — unless a link of this app was accepted meanwhile (`superseded`) |
| 200 with anything else (not JSON, missing fields, `url` on a host outside `allowedHosts`)                                                                                   | Store `done`; status `failed`. Nothing is delivered                                          |
| 404, any other 4xx except 429                                                                                                                                               | Store `done`; status `notFound`                                                              |
| 429 (not retried in this launch), or 5xx / transport failure / timeout after the retries                                                                                    | Status `pending`, `done` not stored: a later launch tries again, until step 3 ends it        |

### Delivery

The deferred link is a resolved link like any other, with `uri` = the answer's `url`, `payload`
and `campaign` read exactly as for [resolving](#resolving), and one new property on **every**
resolved link: `isDeferred` (boolean; `true` here, `false` for links that arrived the normal
way).

- It goes through the same exactly-once pipeline: the normal stream or listener, queued until a
  listener attaches.
- `getInitialLink` **never** returns it: it is not the launch link, and it can arrive seconds
  after the app showed its first screen.
- `done` is stored **before** the link is handed to the pipeline; status `delivered`.
- **A normal link wins.** If a link of this app is accepted at any moment before the deferred
  link has been handed to the pipeline — the launch URL, or one that arrives while the request
  is in flight — the deferred link is dropped (`superseded`): one launch, one navigation. It is
  not queued behind the other link and it is never delivered later.
- No `app_open` event is sent for a deferred link, whatever `reportAppOpens` says. The server
  counted the redemption itself.
- After `dispose`/`stop` nothing is delivered; `done` is stored if the answer had arrived.

### Crashes and the once-only guarantee

The rule is **at most once**: a deferred link may be lost, it is never delivered twice and the
check never repeats without end.

- `attempts` is written before the sources are asked, so a launch that crashes or is killed at
  any point of the check has used up one of three attempts. An app that crashes on every start
  stops asking after the third.
- `done` is not written before the request, because the first launch after an install is
  exactly when a network call fails for ordinary reasons (captive portal, the person is in a
  lift). Writing it first would turn every such launch into a lost link.
- `done` is written before delivery, so a crash after the app received the link cannot deliver
  it again. A crash between the answer and that write costs an attempt; the next launch sends
  the same id again and the server either repeats its answer (within two minutes) or refuses it
  (after) — one delivery or none.
- The server is the second lock: an id is redeemable once, so even an SDK that lost its state
  (cleared app data) gets `notFound`.

### Status and errors

Most people who install an app never tapped a link. A check that finds nothing is the normal
case, and a check that fails is not a failed link. So **nothing in this section is reported to
`onError`, nothing throws, and nothing is shown**. For developers and QA the SDK exposes
`deferredLinkStatus`, readable at any time and observable where the platform has a natural way:

| Status           | Meaning                                                                                            | Final |
| ---------------- | -------------------------------------------------------------------------------------------------- | ----- |
| `disabled`       | `deferredLinks` is off                                                                             | —     |
| `checking`       | The check is running in this launch                                                                | no    |
| `pending`        | This launch could not finish the check (store or network not reachable); a later launch tries      | no    |
| `notFirstLaunch` | The check was finished in an earlier launch, or the app was updated since it was installed         | yes   |
| `unavailable`    | No source is configured, or none can ever answer on this device                                    | yes   |
| `noReferrer`     | The store returned a referrer without a click id of ours (the usual result of an ordinary install) | yes   |
| `notFound`       | The server refused the id: unknown, older than 24 hours, already used, or not this application's   | yes   |
| `superseded`     | A normal link of this app arrived first                                                            | yes   |
| `delivered`      | The link was handed to the delivery pipeline                                                       | yes   |
| `failed`         | Three launches could not finish the check, or the server's answer was unusable                     | yes   |

SDKs may log the status in debug builds. They must never log the referrer or the click id.

### Sources shipped with the Android SDK

All three are shipped **as code** in the Android SDK (and in `androidMain` of the Kotlin
Multiplatform SDK). None adds a dependency to the published artifact: the two that need a
vendor library are compiled against it as `compileOnly`, exactly like Play Integrity, and answer
"unavailable" when the app did not add it. Reasons for shipping code instead of documented
snippets: Samsung's library is on Maven Central and Huawei's mechanism needs no library, so
there is nothing an app could add that we cannot compile against; and three copies of the
response-code mapping in app code would be three places to get it wrong.

| Source                             | Store                | The app adds                                                                                                                                                                                                                                 | Standing                                                                                                                                                                                                                                                       |
| ---------------------------------- | -------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `PlayInstallReferrerSource`        | Google Play          | `com.android.installreferrer:installreferrer:2.2` (Google's Maven repository)                                                                                                                                                                | From Google's documentation. Not device-tested                                                                                                                                                                                                                 |
| `GalaxyStoreInstallReferrerSource` | Samsung Galaxy Store | `store.galaxy.samsung.installreferrer:samsung_galaxystore_install_referrer:4.0.0` (Maven Central)                                                                                                                                            | **Unverified.** API read from the library's published sources; Galaxy Store 4.5.20.1 or later. Whether a referrer put on an `https` store link reaches the app is known from third parties only                                                                |
| `AppGalleryInstallReferrerSource`  | Huawei AppGallery    | Nothing to compile. Possibly a `<queries>` entry for the provider authority `com.huawei.appmarket.commondata` and the permission `com.huawei.appmarket.service.commondata.permission.GET_COMMON_DATA` — **unverified**, not in Huawei's page | **Unverified.** From Huawei's AppGallery Connect pages; AppGallery 10.5.0.300 or later. Huawei states the provider "may be called only after user consent"; the app is responsible for that, and enables `deferredLinks` (or adds this source) only afterwards |

`PlayInstallReferrerSource` and `GalaxyStoreInstallReferrerSource` use the vendor's
`InstallReferrerClient` (`com.android.installreferrer.api` and
`com.samsung.android.sdk.sinstallreferrer.api`; same shape, same response codes):

| Result of `startConnection`                                                | Answer of the source                                   |
| -------------------------------------------------------------------------- | ------------------------------------------------------ |
| `OK` (0)                                                                   | The string from `ReferrerDetails.getInstallReferrer()` |
| `SERVICE_UNAVAILABLE` (1), `SERVICE_DISCONNECTED` (-1), a remote exception | Unavailable for now                                    |
| `FEATURE_NOT_SUPPORTED` (2), `DEVELOPER_ERROR` (3)                         | Unavailable                                            |
| The vendor library is not in the app (class not found)                     | Unavailable                                            |

The connection is always closed with `endConnection()`. Of `ReferrerDetails` only the referrer
string is read; the click and install timestamps are not.

`AppGalleryInstallReferrerSource` queries the content provider
`content://com.huawei.appmarket.commondata/item/5` with the app's own package name as the single
selection argument (`query(uri, null, null, arrayOf(packageName), null)`) and returns column
**5** of the first row as the referrer (Huawei: "Referrer parameter set in the download link").
No provider, a null cursor, no row or fewer than six columns → unavailable; a
`SecurityException` → unavailable; any other failure → unavailable for now. No other column is
read.

A recommended default for apps distributed in all three stores is the list
`[Play, GalaxyStore, AppGallery]`: a source whose store did not install the app returns no id
and the next one is asked.

### Cross-platform SDKs

| SDK                              | How it gets the referrer                                                                                                                                                                                 |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Android (Kotlin)                 | The shipped sources                                                                                                                                                                                      |
| Kotlin Multiplatform             | The same sources in `androidMain`; the flow in common code. On other targets the option is ignored (status `disabled`)                                                                                   |
| Flutter, React Native, Capacitor | No native code of ours: `installReferrerSources` takes functions the app fills from a plugin of its choice, returning the three answers above. `androidPackageName` is required. Off Android: `disabled` |
| Unity, .NET MAUI, Unreal         | The same source interface as a delegate the app fills (0.3.0 ships no store source of its own)                                                                                                           |
| iOS (Swift)                      | No install referrer. `deferredLinks` enables only the flows of [Deferred links on iOS](#deferred-links-on-ios-optional)                                                                                  |
| JavaScript (`@strybok/js`)       | The same functions as React Native and Capacitor, which wrap it. On a platform that is neither Android nor iOS (a web page): `disabled`                                                                  |

Every SDK that implements the option implements the whole flow, state and status of this
section, with tests against a fake source and the stub transport: disabled by default; start
completes while a source blocks; attempts counted before the source is asked; three attempts,
then `failed`; every status; a normal link wins before and during the request; `done` before
delivery; restart after each step; a `url` outside `allowedHosts` is dropped; malformed and
still-encoded referrers; several sources in order; no `app_open`.

### What deferred links send and read

With `deferredLinks` on, the SDK reads the store's install referrer at most three times in the
life of an installation and sends **only** the click id found in it, the app's package name and
the SDK version. The id is random, made by the service when the link was tapped, usable once
and for 24 hours, and says nothing about the device or the person. Reading the referrer is
access to information stored on the device; whether the app needs the person's consent first is
for the app developer to decide, and the SDK supports that by doing nothing until the app
switches `deferredLinks` on — which SDKs must allow after start, for example after a consent
screen (`enableDeferredLinks()` or an equivalent setter; calling it runs the flow once).

**Older SDK versions.** A version without the `deferredLinks` option never asks a store and
never calls the redeem endpoint; it keeps working unchanged whether or not the owner switched
the feature on. For its users the click ids in store URLs are simply never used and expire. A
version that does not know `isDeferred` cannot receive a deferred link, so the property needs no
default on old link objects. The redeem answer may gain fields; SDKs ignore fields they do not
know, as for resolving.

## Deferred links on iOS (optional)

_Added on 2026-11-24 (decision 0019). Implemented,
covered by unit tests only, by iOS (Swift) 0.4.0 and Kotlin Multiplatform 0.4.0 (`iosMain`), and
by Flutter, JavaScript, React Native, Capacitor, Unity, .NET and Unreal 0.3.0, where the app
supplies the pasted text and, for flow B, the device family and language — see
[Which SDKs implement deferred links](#which-sdks-implement-deferred-links)._

The App Store passes nothing from a link to a newly installed app. There are two flows, each
switched on per application by the owner **and** in the app by the developer, both off by
default, and neither comparable to Android's:

- **A. Pasted link** — the person copies the link on a page before the App Store and pastes it
  in the app. Honest and exact, but it needs two deliberate actions by the person.
- **B. Probabilistic matching** — the server guesses from the network address. Invisible, often
  wrong or impossible, and **forbidden by Apple's rules**. Specified because owners may choose
  it; not recommended.

Nothing here has run on a device. `deferredLinks` on an iOS SDK enables flow A; flow B also
needs `allowProbabilisticMatching`. With `deferredLinks` off, an iOS SDK does nothing of this
section.

State is the same pair as on Android, per installation: `attempts` (used by flow B only) and
`done`. **Once either flow delivered a link, or flow A got a definite answer from the server,
`done` is stored and neither flow does anything again.**

### Flow A: pasted link

On the server (owner setting `iosDeferredClipboard`): an iOS visitor who would be sent to the
App Store sees a page with a "Copy link and get the app" button. It copies
`https://<link host>/<slug>?strybok_click=dc_<32 characters>` — the link itself plus a
single-use click id of the same kind as Android's — and opens the App Store.

**The SDK never reads the pasteboard.** No SDK code may call `UIPasteboard`'s `string`, `url`,
`strings`, `urls`, `items`, `detectValues(for:…)` / `detectedValues(for:)` or anything else that
returns pasteboard content. The app obtains the string — from Apple's `UIPasteControl` (the
system paste button, iOS 16 or later: tapping it is the person's consent, and no "Allow Paste"
alert appears) or from a paste action of its own — and hands it to the SDK:

`redeemPastedLink(string)` → one of `delivered`, `notOurs`, `notFound`, `alreadyHandled`,
`disabled`, `failed`.

1. `deferredLinks` is off → `disabled`. `done` is stored → `alreadyHandled`. No request.
2. Trim the string. It must parse as a URL that passes
   ["Which URLs are ours"](#which-urls-are-ours), and the **first** `strybok_click` parameter of
   its query must match `^dc_[0-9A-Za-z]{32}$`. Anything else → `notOurs`: **no request is
   made** and the string is dropped — not stored, not logged, not sent anywhere. (The person
   may have had anything on their clipboard.)
3. `POST {apiBaseUrl}/v1/public/deferred-links/redeem` with

   ```json
   {
     "platform": "ios",
     "clickId": "dc_…",
     "bundleId": "<Bundle.main.bundleIdentifier>",
     "sdkVersion": "<SDK version>"
   }
   ```

   `timeout`, `maxRetries` and `retryDelay` as for resolving; a retry sends the identical body.
   Only the click id is sent, never the pasted URL.

| Outcome                                                                                                           | Result                                                                                  |
| ----------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| 200 with `linkId`, `appId`, `routePath` valid as for resolving, `deferred` = `true`, and `url` a link of this app | Store `done`, then deliver; `delivered`                                                 |
| 200 with anything else                                                                                            | Store `done`; `failed`                                                                  |
| 404, any other 4xx except 429                                                                                     | Store `done`; `notFound` (unknown, older than 24 hours, already used, another app's id) |
| 429, 5xx, transport failure, timeout — after the retries                                                          | `failed`; `done` is **not** stored, so the person can tap Paste again                   |

Delivery is as on Android: a resolved link with `uri` = the answer's `url` (without the click
id), `isDeferred` = `true`, through the normal exactly-once pipeline; `getInitialLink` never
returns it; no `app_open` is sent. One difference: **a pasted link is delivered even if a normal
link was delivered earlier in this launch** — pasting was the person's own, explicit action.

**Optional: is there anything to paste?** An SDK may offer `pasteboardMayContainLink()` so the
app can decide whether to show its paste button. It must be implemented with
`UIPasteboard.general.detectPatterns(for: [.probableWebURL], completionHandler:)` (iOS 14 or
later; the `async` form is `detectedPatterns(for:)`, iOS 15 or later), which Apple documents as
determining whether the pasteboard matches the patterns "without notifying the user": it reads
no content and shows no banner or alert. It answers only "probably a web URL" — it cannot know
whether the URL is ours — and returns `false` when `done` is stored or `deferredLinks` is off.
SDKs may leave this method out.

What the app developer is told: place the paste control on the first screen or in onboarding,
with a sentence that says what it does; describe it in the App Review notes; do not paste
automatically.

Hybrid SDKs (Flutter, React Native, Capacitor, .NET MAUI, Unity) expose `redeemPastedLink` with
a plain string argument; the paste button is the app's, from whatever plugin it uses. They never
read the clipboard either.

### Flow B: probabilistic matching

On the server (owner setting `iosDeferredMatching`, plus `allowProbabilisticMatch` on each
link): when an iOS visitor is sent to the App Store, a keyed hash of their network address,
device family and primary language is kept for 15 minutes to 6 hours. The first launch is
matched against it.

**This is fingerprinting in Apple's terms.** The App Store Review Guidelines and the Apple
Developer Program License Agreement forbid deriving data from a device to identify it, with or
without the user's consent. An SDK that implements this flow must say so in its documentation
next to the option, and must keep it off unless the developer sets
`allowProbabilisticMatching` explicitly. A match is a guess: on a shared network it can be
**another person's link**.

Runs only when `deferredLinks` **and** `allowProbabilisticMatching` are on, at most once per
process, after `initialize`/`start` has completed everything it does today; nothing waits for
it.

1. `done` is stored → status `notFirstLaunch`. Stop.
2. **Clipboard first.** If the SDK implements `pasteboardMayContainLink()` and it answers
   `true`, the SDK does not match yet: status `awaitingPaste`. Matching then starts only when
   the app calls `redeemPastedLink` and gets `notOurs`, or calls `skipPastedLink()` (the person
   moved on without pasting). If the pasted link is ours, `done` is stored by flow A and
   matching never runs.
3. `attempts` is 3 or more → store `done`; status `failed`. Stop.
4. Store `attempts + 1`, before the request (the same reasoning as on Android:
   [Crashes](#crashes-and-the-once-only-guarantee)).
5. If a link of this app has been accepted since the process started → store `done`; status
   `superseded`. No request.
6. `POST {apiBaseUrl}/v1/public/deferred-links/match` with

   ```json
   {
     "platform": "ios",
     "bundleId": "<Bundle.main.bundleIdentifier>",
     "deviceFamily": "iphone",
     "locale": "en-GB",
     "sdkVersion": "<SDK version>"
   }
   ```

   - `deviceFamily`: `iphone`, `ipad` or `ipod`, from `UIDevice.current.userInterfaceIdiom`
     (`.phone` is `ipod` when `UIDevice.current.model` starts with `iPod`). Any other idiom
     (Mac, TV, Vision) → store `done`; status `unavailable`; no request.
   - `locale`: `Locale.preferredLanguages.first`. No such value → `unavailable`, as above.
   - **Nothing else.** No identifier for vendor or advertising, no device model or name, no
     screen size, no OS version, no time zone, no carrier. The server takes the network address
     from the connection. The strict schema refuses any other field.

| Outcome                                                                                                                                          | Result                                                                                       |
| ------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------- |
| 200 with `linkId`, `appId`, `routePath` valid as for resolving, `deferred` = `true`, `matched` = `"probabilistic"`, and `url` a link of this app | Store `done`, then deliver — unless a link of this app was accepted meanwhile (`superseded`) |
| 200 with anything else                                                                                                                           | Store `done`; status `failed`                                                                |
| 404, any other 4xx except 429                                                                                                                    | Store `done`; status `notFound`. The usual answer: nothing fitted, or more than one did      |
| 429, 5xx, transport failure, timeout — after the retries                                                                                         | Status `pending`; a later launch tries again, until step 3 ends it                           |

A match is used up by the request that gets it. There is no grace period as for click ids: if
the answer is lost on the way, a retry gets 404 and the link is gone.

Delivery: as on Android (`isDeferred` = `true`, exactly once, never from `getInitialLink`, no
`app_open`, a normal link wins), plus a second property on every resolved link:
`isProbabilisticMatch` (boolean; `true` only here). **Apps must check it**: a probabilistic link
may belong to someone else, so it must never by itself sign anyone in, accept an invitation,
apply a personal discount or show personal content. The SDK documentation says this next to the
property.

Status values are those of [Android](#status-and-errors) plus `awaitingPaste`. Nothing is
reported to `onError`.

The iOS SDK (Swift) and Kotlin Multiplatform (`iosMain`) implement both flows natively. Flutter,
JavaScript, React Native, Capacitor, Unity, .NET and Unreal have no native code of ours: the app
passes the pasted text to `redeemPastedLink`, flow B needs `deviceFamily` and `locale` from the
app (two option callbacks; .NET MAUI and Unity fill them from the framework), and the
`pasteboardMayContainLink` step is skipped unless the app supplies it.

### What deferred links on iOS send and read

Flow A: the SDK reads nothing by itself. It receives a string the person pasted, sends the click
id found in it together with the bundle id and the SDK version, and drops the string. Flow B:
the SDK sends the bundle id, the device family, the first preferred language and the SDK
version, and the server additionally uses the connection's IP address — which is processing of
personal data, and identification of a device from its properties in Apple's sense. The server
keeps only a keyed hash and deletes it within hours (see privacy). Whether the
app needs the person's consent, what its privacy policy and App Store privacy labels must say,
and whether App Review accepts it, are the app developer's responsibility; the SDK supports a
consent step by doing nothing until `deferredLinks` is switched on.

**Older SDK versions.** An iOS SDK without `deferredLinks` never calls either endpoint. A person
who pastes the copied link into such an app gets nothing; a person who opens the copied link in
a browser gets an ordinary open of the link (the `strybok_click` parameter is ignored there, and
SDKs drop it from the URL like every other non-campaign parameter).

## Which SDKs implement deferred links

All versions below are **unreleased** at the time of writing (2026-11-25) and are covered by unit
tests against fake sources and a stub transport only. Nothing has run on a device, against a
real store, with a real pasteboard or against the production API, and the server side is not
deployed to production yet. Everything is optional and off by default.

| SDK                                     | Version | Android (store referrer)                                                                                                                                                        | iOS flow A (pasted link)                                 | iOS flow B (probabilistic matching)                                     |
| --------------------------------------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------- | ----------------------------------------------------------------------- |
| Android (Kotlin)                        | 0.4.0   | Yes, with the shipped sources for Google Play, Galaxy Store and AppGallery; the app adds the store's library as a dependency                                                    | —                                                        | —                                                                       |
| Kotlin Multiplatform                    | 0.4.0   | Yes, the same sources in `androidMain`                                                                                                                                          | Yes (`redeemPastedLink`)                                 | Yes, only with `allowProbabilisticMatching`                             |
| iOS (Swift)                             | 0.4.0   | —                                                                                                                                                                               | Yes (`redeemPastedLink`)                                 | Yes, only with `allowProbabilisticMatching`                             |
| Flutter                                 | 0.3.0   | The flow; the app supplies the referrer string and the state store                                                                                                              | The flow; the app supplies the pasted text and the store | The flow; the app also supplies device family and language              |
| JavaScript, React Native, Capacitor     | 0.3.0   | The flow; the app supplies the referrer string and the state store                                                                                                              | The flow; the app supplies the pasted text and the store | The flow; the app also supplies device family and language              |
| Unity, .NET (`Strybok`, `Strybok.Maui`) | 0.3.0   | The flow; the app supplies the referrer string (state: `PlayerPrefs` in Unity and MAUI `Preferences` in `Strybok.Maui` by default; the plain .NET package has no default store) | The flow; the app supplies the pasted text               | The flow; device family and language are filled by default, replaceable |
| Unreal Engine                           | 0.3.0   | The flow; the game supplies the referrer string from C++                                                                                                                        | The flow; the game supplies the pasted text              | The flow; the game supplies device family and language from C++         |

Of the three store sources only Google Play's is written against the vendor's documentation;
Galaxy Store and AppGallery are **unverified** (see
[Sources shipped with the Android SDK](#sources-shipped-with-the-android-sdk)). Flow B also
needs the service to have a matching secret, the owner's setting and a per-link permission; a
service without the secret answers that matching is not available. No flow is automatic on iOS:
flow A needs the person to copy and paste, flow B guesses and is forbidden by Apple's rules.

## Errors

| Kind                  | When                                                                       |
| --------------------- | -------------------------------------------------------------------------- |
| `UnsupportedLink`     | `resolve` was given a URL that is not ours (no network call).              |
| `LinkNotFound`        | 404: unknown, disabled or expired link.                                    |
| `Network`             | The request could not be completed, after all retries.                     |
| `Timeout`             | No answer in time, after all retries.                                      |
| `Server`              | Unexpected status or body; carries the status code.                        |
| `Unauthorized`        | `createLink` only, see [Creating links](#creating-links).                  |
| `RouteNotAllowed`     | `createLink` only.                                                         |
| `RateLimited`         | `createLink` only.                                                         |
| `Validation`          | `createLink` only.                                                         |
| `AttestationRequired` | `createLink` only, SDKs with [attestation](#attestation-optional) support. |
| `AttestationFailed`   | `createLink` only, SDKs with attestation support.                          |

Every error carries the URL being handled when known. The deferred-link check reports no errors
at all, only a [status](#status-and-errors).

## Privacy

SDKs collect no device identifiers, no advertising IDs and no fingerprint. The only data sent is
the link host and slug plus the five `utm_*` campaign parameters of the tapped URL when they pass
the character rule (resolve) — never any other query parameter or the fragment —, the event
above, and — only when the app calls `createLink` —
the client key and the route, payload, title, description and image URL the app passed in. Apps
must not put personal data in a route or payload: whoever receives the link can read them. With
`attestation` switched on, `createLink` also sends what
[Attestation](#what-attestation-sends) lists. With `deferredLinks` switched on, the SDK also
reads the store's install referrer and sends what
[Deferred links](#what-deferred-links-send-and-read) lists (Android), or sends what
[Deferred links on iOS](#what-deferred-links-on-ios-send-and-read) lists. The one exception to
the first sentence of this section is iOS probabilistic matching, which exists only behind
`allowProbabilisticMatching`: there the server identifies the device by its network address and
two coarse properties.
