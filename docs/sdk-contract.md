# SDK contract

Every Strybok SDK behaves the same way. The Flutter SDK
([github.com/strybok/strybok-flutter](https://github.com/strybok/strybok-flutter)) is the
reference implementation; its test suite is the reference test list. This page is the
language-neutral version of it.

Deferred deep linking (restoring a link after a store install) is **not** supported by any SDK.

## Configuration

| Option           | Default  | Meaning                                                                          |
| ---------------- | -------- | -------------------------------------------------------------------------------- |
| `apiBaseUrl`     | required | Base URL of the API, e.g. `https://api.strybok.com`. A path prefix is preserved. |
| `allowedHosts`   | required | Link hostnames of this app, compared lower-case. Other hosts are ignored.        |
| `timeout`        | 5 s      | Time allowed for one resolve attempt.                                            |
| `maxRetries`     | 2        | Extra attempts after a timeout, network error or 5xx.                            |
| `retryDelay`     | 400 ms   | Pause before the first retry; doubles for each further retry.                    |
| `dedupeWindow`   | 2 s      | The identical URL received again within this window is dropped.                  |
| `reportAppOpens` | `true`   | Send a best-effort `app_open` event after a link was resolved.                   |
| `onError`        | none     | Receives errors for our links that could not be resolved.                        |

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

## Errors

| Kind              | When                                                          |
| ----------------- | ------------------------------------------------------------- |
| `UnsupportedLink` | `resolve` was given a URL that is not ours (no network call). |
| `LinkNotFound`    | 404: unknown, disabled or expired link.                       |
| `Network`         | The request could not be completed, after all retries.        |
| `Timeout`         | No answer in time, after all retries.                         |
| `Server`          | Unexpected status or body; carries the status code.           |

Every error carries the URL being handled when known.

## Privacy

SDKs collect no device identifiers, no advertising IDs and no fingerprint. The only data sent is
the link host and slug (resolve) and the event above.
