# SDK contract

Every Strybok SDK behaves the same way. The Flutter SDK
([github.com/strybok/strybok-flutter](https://github.com/strybok/strybok-flutter)) is the
reference implementation; its test suite is the reference test list. This page is the
language-neutral version of it.

Deferred deep linking (restoring a link after a store install) is **not** supported by any SDK.

## Configuration

| Option           | Default  | Meaning                                                                                                                                                                                                 |
| ---------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `apiBaseUrl`     | required | Base URL of the API, e.g. `https://api.strybok.com`. A path prefix is preserved.                                                                                                                        |
| `allowedHosts`   | required | Link hostnames of this app, compared lower-case. Other hosts are ignored.                                                                                                                               |
| `timeout`        | 5 s      | Time allowed for one resolve attempt.                                                                                                                                                                   |
| `maxRetries`     | 2        | Extra attempts after a timeout, network error or 5xx.                                                                                                                                                   |
| `retryDelay`     | 400 ms   | Pause before the first retry; doubles for each further retry.                                                                                                                                           |
| `dedupeWindow`   | 2 s      | The identical URL received again within this window is dropped.                                                                                                                                         |
| `reportAppOpens` | `true`   | Send a best-effort `app_open` event after a link was resolved.                                                                                                                                          |
| `onError`        | none     | Receives errors for our links that could not be resolved.                                                                                                                                               |
| `clientKey`      | none     | Publishable key `pk_…` of the application. Only needed for `createLink`.                                                                                                                                |
| `attestation`    | `false`  | Prove to the server that this is the genuine app before creating links. Only SDKs that support it have this option (Android also needs `cloudProjectNumber`), see [Attestation](#attestation-optional). |

The source of incoming URLs (`LinkSource`: a launch URL plus a stream of later URLs) and the HTTP
transport are replaceable, so the SDK can be tested without a device or a network.

## Which URLs are ours

A URL is a Strybok link of this app only when **all** of these hold:

- scheme is `https`;
- host, lower-cased, is in `allowedHosts`;
- no user info (`user:pass@`);
- no port, or port 443;
- path matches `^/[A-Za-z0-9_-]{1,64}$` (exactly one slug segment, no trailing slash).

Query and fragment are allowed and ignored. Anything else is dropped silently by the automatic
flow (no error, no network call) and rejected with `UnsupportedLink` by `resolve`.

## Resolving

`GET {apiBaseUrl}/v1/public/resolve?url={lookup}` with `Accept: application/json`, where `lookup`
is `https://{lower-cased host}{path}` — query and fragment are **never** sent, they may hold
private data.

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

A resolved link has: the original `uri` (as received, with query), `linkId`, `appId`, `routePath`,
`payload`. `routePath` and `payload` are untrusted data chosen by whoever created the link: apps
map them onto routes they already know.

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
  "sdkVersion": "<SDK version, at most 32 characters>"
}
```

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

**Which SDKs.** Attestation needs native platform code. It is planned for the native iOS SDK and
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

Every error carries the URL being handled when known.

## Privacy

SDKs collect no device identifiers, no advertising IDs and no fingerprint. The only data sent is
the link host and slug (resolve), the event above, and — only when the app calls `createLink` —
the client key and the route, payload, title, description and image URL the app passed in. Apps
must not put personal data in a route or payload: whoever receives the link can read them. With
`attestation` switched on, `createLink` also sends what
[Attestation](#what-attestation-sends) lists.
