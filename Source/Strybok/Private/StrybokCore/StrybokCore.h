// Strybok SDK core: every rule of docs/sdk-contract.md in portable C++17.
//
// This file and StrybokCore.cpp use the C++ standard library only (no Unreal headers), so the same
// code is compiled into the Unreal module and into the standalone test runner in Tests/.
//
// Constraints the code keeps on purpose, because Unreal builds modules this way:
//   * no exceptions (no throw / try, no std::optional::value(), no std::stoi);
//   * no RTTI (no dynamic_cast / typeid);
//   * no <regex>, no iostreams.
//
// Threading: a LinkEngine is single-threaded. Every call into it, and every callback handed to the
// transport and the scheduler, must happen on the same thread (the game thread in Unreal).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace strybok
{

/** Version reported with analytics events. */
extern const char* const kSdkVersion;

// ---------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------

enum class ErrorKind
{
	/** `Resolve` was given a URL that is not a link of this app. No network call was made. */
	UnsupportedLink,
	/** 404: unknown, disabled or expired link. */
	LinkNotFound,
	/** The request could not be completed, after all retries. */
	Network,
	/** No answer in time, after all retries. */
	Timeout,
	/** Unexpected status code or body; `statusCode` is set. */
	Server,
	/** Creating links only: no client key configured (no network call), or 401. */
	Unauthorized,
	/** Creating links only: 403, the route is not covered by the application's route patterns. */
	RouteNotAllowed,
	/** Creating links only: 429; `retryAfterSeconds` is set when the server sent `Retry-After`. */
	RateLimited,
	/** Creating links only: rejected before sending, or 400 / 413 / 422 from the server. */
	Validation,
};

/** One entry of the server's `error.details.issues` for a `Validation` error. */
struct ValidationIssue
{
	/** Which field, e.g. `title` or `payload.a`; may be empty. */
	std::string path;
	std::string message;
};

struct LinkError
{
	ErrorKind kind = ErrorKind::Server;
	std::string message;
	/** The URL that was being handled; empty for errors of creating a link. */
	std::string url;
	/** HTTP status of the answer when there was one; 0 otherwise. */
	int statusCode = 0;
	/** `RouteNotAllowed`: the route that was refused. */
	std::string routePath;
	/** `RateLimited`: seconds from the `Retry-After` header; -1 when the server sent none. */
	std::int64_t retryAfterSeconds = -1;
	/** `Validation` from the server: its `error.details.issues`, if any. */
	std::vector<ValidationIssue> issues;
};

/**
 * A link resolved by the API. `routePath` and `payloadJson` are untrusted data chosen by whoever
 * created the link: map them onto routes the app already knows.
 */
struct ResolvedLink
{
	/** The URL exactly as received, including query and fragment. */
	std::string uri;
	std::string linkId;
	std::string appId;
	/** Always starts with a single `/`. */
	std::string routePath;
	/** The `payload` JSON object as text; `{}` when the response had none or it was not an object. */
	std::string payloadJson = "{}";
};

struct Config
{
	/** Base URL of the API, e.g. `https://api.strybok.com`. A path prefix is preserved. */
	std::string apiBaseUrl;
	/** Link hostnames of this app; compared lower-case. */
	std::vector<std::string> allowedHosts;
	/** Time allowed for one resolve attempt. */
	std::int64_t timeoutMs = 5000;
	/** Extra attempts after a timeout, network error or 5xx. */
	int maxRetries = 2;
	/** Pause before the first retry; doubles for each further retry. */
	std::int64_t retryDelayMs = 400;
	/** The identical URL received again within this window is dropped. */
	std::int64_t dedupeWindowMs = 2000;
	/** Send a best-effort `app_open` event after a link was resolved. */
	bool reportAppOpens = true;
	/** `ios`, `android` or `unknown`. */
	std::string platform = "unknown";
	/** Reported with events, truncated to 32 characters. Empty means `kSdkVersion`. */
	std::string sdkVersion;
	/**
	 * Publishable key `pk_...` of the application. Only needed for creating links; it is not a
	 * secret. Never put a secret API key (`lb_...`) here.
	 */
	std::string clientKey;
};

/** What `LinkCreator::Create` sends. Only `routePath` is required; an empty string means "not given". */
struct CreateLinkParams
{
	/** In-app route the link opens: starts with `/`, not with `//`, at most 512 characters. */
	std::string routePath;
	/** A JSON object as text, e.g. `{"productId":42}`. Empty: no payload. */
	std::string payloadJson;
	std::string title;
	std::string description;
	/** Preview image; the server requires a public HTTPS URL. */
	std::string imageUrl;
};

/** A link made by `LinkCreator::Create`. */
struct CreatedLink
{
	/** What the user shares; always starts with `https://`. */
	std::string url;
	std::string slug;
	std::string routePath;
	std::string linkId;
};

struct HttpRequest
{
	std::string method;
	std::string url;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
};

struct HttpResponse
{
	/** False when no HTTP response was received (offline, DNS, connection reset, ...). */
	bool transportOk = false;
	int statusCode = 0;
	std::string body;
	/** Value of the `Retry-After` response header; empty when the response had none. */
	std::string retryAfter;
};

// ---------------------------------------------------------------------------------------------
// Replaceable environment
// ---------------------------------------------------------------------------------------------

class IHttpTransport
{
public:
	using Callback = std::function<void(const HttpResponse&)>;

	virtual ~IHttpTransport();

	/**
	 * Starts a request and returns a non-zero id for `Cancel`. The callback must be invoked at
	 * most once, on the engine's thread. It may be invoked before `Send` returns.
	 */
	virtual std::uint64_t Send(const HttpRequest& request, Callback onDone) = 0;

	/** Abandons a request. Its callback should not be invoked afterwards (the engine ignores it anyway). */
	virtual void Cancel(std::uint64_t requestId) = 0;
};

class IScheduler
{
public:
	virtual ~IScheduler();

	/** Runs `task` once after `delayMs` on the engine's thread, never before `Schedule` returns. */
	virtual std::uint64_t Schedule(std::int64_t delayMs, std::function<void()> task) = 0;

	virtual void Cancel(std::uint64_t timerId) = 0;
};

class IClock
{
public:
	virtual ~IClock();

	/** Monotonic milliseconds; only differences are used. */
	virtual std::int64_t NowMs() = 0;
};

class IRandom
{
public:
	virtual ~IRandom();

	virtual void Fill(std::uint8_t* bytes, std::size_t count) = 0;
};

// ---------------------------------------------------------------------------------------------
// Pure rules (no state, no I/O)
// ---------------------------------------------------------------------------------------------

/** Trims ASCII whitespace, lower-cases ASCII letters, drops empty entries and duplicates. */
std::vector<std::string> NormalizeHosts(const std::vector<std::string>& hosts);

/** Returns an empty string when `config` is usable, otherwise what is wrong with it. */
std::string ValidateConfig(const Config& config);

/**
 * Whether `url` is a Strybok link of this app: `https`, host in `normalizedHosts`, no user info,
 * no port or port 443, and a path matching `^/[A-Za-z0-9_-]{1,64}$`. Query and fragment are
 * allowed and ignored. `normalizedHosts` must come from `NormalizeHosts`.
 */
bool IsStrybokUrl(const std::string& url, const std::vector<std::string>& normalizedHosts);

/**
 * `https://{lower-cased host}{path}` for a URL accepted by `IsStrybokUrl`; false otherwise.
 * Query and fragment are never part of the result: they may hold private data.
 */
bool BuildLookupUrl(const std::string& url, const std::vector<std::string>& normalizedHosts, std::string& outLookup);

/** Percent-encodes everything except RFC 3986 unreserved characters. */
std::string PercentEncode(const std::string& value);

/** `{apiBaseUrl}/v1/public/resolve?url={encoded lookup}`; trailing slashes of the base are dropped. */
std::string BuildResolveEndpoint(const std::string& apiBaseUrl, const std::string& lookupUrl);

/** `{apiBaseUrl}/v1/public/events`. */
std::string BuildEventsEndpoint(const std::string& apiBaseUrl);

/** Pause before retry number `retryNumber` (1 = first retry): `retryDelayMs * 2^(retryNumber-1)`. */
std::int64_t RetryDelayMs(std::int64_t retryDelayMs, int retryNumber);

/**
 * Validates a 200 body: a JSON object with string `linkId`, `appId` and `routePath`, where
 * `routePath` starts with `/` but not `//`. Fills `outLink` (with `uri` as given) on success.
 */
bool ParseResolveBody(const std::string& uri, const std::string& body, ResolvedLink& outLink);

enum class AttemptAction
{
	Deliver,
	Fail,
	Retry,
};

struct AttemptResult
{
	AttemptAction action = AttemptAction::Fail;
	/** Valid for `Deliver`. */
	ResolvedLink link;
	/** Valid for `Fail` and `Retry` (the error to report if no attempts are left). */
	LinkError error;
};

/** Decides what one HTTP answer to a resolve request means. */
AttemptResult ClassifyResolveResponse(const std::string& uri, const HttpResponse& response);

/** RFC 4122 version 4 UUID text from 16 random bytes (version and variant bits are forced). */
std::string FormatUuidV4(const std::uint8_t bytes[16]);

/** The exact `app_open` event body: `eventId`, `linkId`, `type`, `platform`, `sdkVersion`. */
std::string BuildAppOpenBody(
	const std::string& eventId,
	const std::string& linkId,
	const std::string& platform,
	const std::string& sdkVersion);

// --- creating links ---

/** Like `ValidateConfig`, but only what creating links needs: `allowedHosts` is not looked at. */
std::string ValidateCreateLinkConfig(const Config& config);

/** `{apiBaseUrl}/v1/public/links`. */
std::string BuildLinksEndpoint(const std::string& apiBaseUrl);

/**
 * `clientKey` without surrounding ASCII whitespace. Empty when it is missing or cannot be sent as
 * a header value (contains spaces, control or non-ASCII characters): such a key is never valid.
 */
std::string NormalizeClientKey(const std::string& clientKey);

/**
 * The checks made before any network call, and the request body. Returns false and says why in
 * `outProblem` when `routePath` does not start with `/`, starts with `//` or is longer than 512
 * characters, or when `payloadJson` is given and is not a JSON object. Fields that were not given
 * are left out of the body; nothing else is validated here (the server does that).
 */
bool BuildCreateLinkBody(const CreateLinkParams& params, std::string& outBody, std::string& outProblem);

/**
 * Validates a 200 / 201 body: a JSON object with string `linkId`, `slug`, `routePath` and a `url`
 * string starting with `https://`.
 */
bool ParseCreateLinkBody(const std::string& body, CreatedLink& outLink);

/** Whole seconds from a `Retry-After` value; -1 when it is empty or not a plain number of seconds. */
std::int64_t ParseRetryAfterSeconds(const std::string& value);

struct CreateAttemptResult
{
	/** `Deliver` means the link was created (or already existed). */
	AttemptAction action = AttemptAction::Fail;
	/** Valid for `Deliver`. */
	CreatedLink link;
	/** Valid for `Fail` and `Retry` (the error to report if no attempts are left). */
	LinkError error;
};

/** Decides what one HTTP answer to a create-link request means. */
CreateAttemptResult ClassifyCreateLinkResponse(const std::string& routePath, const HttpResponse& response);

// ---------------------------------------------------------------------------------------------
// Minimal strict JSON reader (RFC 8259), enough to validate API responses
// ---------------------------------------------------------------------------------------------

namespace json
{

enum class Type
{
	Null,
	Bool,
	Number,
	String,
	Array,
	Object,
};

struct Value
{
	// Declared here and defaulted in the .cpp: `members` holds pairs of this (still incomplete)
	// type, and libstdc++ rejects implicit special members instantiated inside the class.
	Value();
	Value(const Value& other);
	Value(Value&& other) noexcept;
	Value& operator=(const Value& other);
	Value& operator=(Value&& other) noexcept;
	~Value();

	Type type = Type::Null;
	bool boolean = false;
	/** Decoded text for `String`; the literal as written for `Number`. */
	std::string text;
	std::vector<Value> items;
	std::vector<std::pair<std::string, Value>> members;
	/** Byte range of this value in the parsed text. */
	std::size_t begin = 0;
	std::size_t end = 0;

	/** The last member with this key (like most JSON decoders), or null. */
	const Value* Find(const std::string& key) const;
};

/** Parses a whole document. Nesting deeper than 64 levels is rejected. */
bool Parse(const std::string& text, Value& outValue);

/** Writes `value` as a JSON string literal, including the quotes. */
std::string Quote(const std::string& value);

} // namespace json

// ---------------------------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------------------------

/**
 * Receives URLs, resolves ours through the API and delivers each resolved link exactly once.
 *
 * The transport, scheduler, clock and random source must outlive the engine. All callbacks are
 * invoked on the engine's thread. Destroying the engine is the same as `Stop`.
 */
class LinkEngine
{
public:
	using LinkHandler = std::function<void(const ResolvedLink&)>;
	using ErrorHandler = std::function<void(const LinkError&)>;
	/** Exactly one of the two pointers is non-null. They are valid only during the call. */
	using ResolveCallback = std::function<void(const ResolvedLink* link, const LinkError* error)>;
	/** `link` is null when there is no undelivered launch link. Valid only during the call. */
	using InitialLinkCallback = std::function<void(const ResolvedLink* link)>;

	/** `random` may be null: a `std::random_device` based source is used then. */
	LinkEngine(Config config, IHttpTransport& transport, IScheduler& scheduler, IClock& clock, IRandom* random = nullptr);
	~LinkEngine();

	LinkEngine(const LinkEngine&) = delete;
	LinkEngine& operator=(const LinkEngine&) = delete;

	/** Receives resolved links while listening. */
	void SetLinkHandler(LinkHandler handler);

	/** Receives errors for our links that could not be resolved (the contract's `onError`). */
	void SetErrorHandler(ErrorHandler handler);

	/**
	 * Whether somebody is ready to receive links. While false, resolved links are queued; switching
	 * to true delivers the queue through the link handler, in order.
	 */
	void SetListening(bool listening);
	bool IsListening() const;

	/**
	 * Optional: asked right before every delivery whether somebody is listening at that very
	 * moment, instead of trusting the last `SetListening` value. For wrappers that cannot observe
	 * listeners coming and going (they poll and call `SetListening(true)` to flush the queue).
	 */
	void SetListeningProbe(std::function<bool()> probe);

	/**
	 * Starts accepting URLs (`HandleUrl`) and then handles the launch URL, if any. `onReady` runs
	 * once the launch link has been resolved or has failed. Calling `Start` again is harmless: the
	 * launch URL of a later call is ignored and `onReady` runs when the first start is ready.
	 */
	void Start(const std::string* launchUrl, std::function<void()> onReady = nullptr);

	bool IsStarted() const;
	/** True once the launch link has been handled. */
	bool IsReady() const;
	bool IsStopped() const;

	/** A URL received while running. Ignored before `Start`, after `Stop`, and when it is not ours. */
	void HandleUrl(const std::string& url);

	/**
	 * The launch link, if it has not been delivered through the link handler yet. Hands it out at
	 * most once and removes it from the queue. Waits for `Start` to become ready.
	 */
	void GetInitialLink(InitialLinkCallback callback);

	/** Synchronous variant: false when not ready yet or when there is no undelivered launch link. */
	bool TakeInitialLink(ResolvedLink& outLink);

	/**
	 * Resolves `url` without touching the queue and without an `app_open` event. Fails with
	 * `UnsupportedLink` (through the callback, before returning) when the URL is not ours.
	 * Returns false, without calling back, when the engine is stopped.
	 */
	bool Resolve(const std::string& url, ResolveCallback callback);

	bool IsStrybokUrl(const std::string& url) const;

	/** Number of resolved links waiting for a listener. */
	std::size_t PendingCount() const;

	/**
	 * Stops everything. Nothing is delivered afterwards, including links that were still
	 * resolving; callbacks of unfinished `Resolve`, `Start` and `GetInitialLink` calls are dropped.
	 */
	void Stop();

private:
	class Impl;
	std::shared_ptr<Impl> impl_;
};

/**
 * Creates short links to screens of this app (the contract's `createLink`, the "Share" button).
 *
 * Independent of `LinkEngine` on purpose: it is never part of starting, resolving or delivering,
 * needs no `allowedHosts` and can be used whether or not an engine exists. It only does something
 * when `Create` is called. Uses `apiBaseUrl`, `clientKey`, `timeoutMs`, `maxRetries` and
 * `retryDelayMs` of the config.
 *
 * The transport and the scheduler must outlive the creator. Same threading rule as `LinkEngine`.
 * Destroying the creator is the same as `Stop`.
 */
class LinkCreator
{
public:
	/** Exactly one of the two pointers is non-null. They are valid only during the call. */
	using Callback = std::function<void(const CreatedLink* link, const LinkError* error)>;

	LinkCreator(Config config, IHttpTransport& transport, IScheduler& scheduler);
	~LinkCreator();

	LinkCreator(const LinkCreator&) = delete;
	LinkCreator& operator=(const LinkCreator&) = delete;

	/**
	 * Asks the service for a link to `params.routePath`. The callback gets the link or one error:
	 * `Unauthorized` (no client key: no network call; or 401), `Validation` (bad route or payload:
	 * no network call; or 400 / 413 / 422), `RouteNotAllowed` (403), `RateLimited` (429), and
	 * `Network` / `Timeout` / `Server` after the retries. Errors found before sending are reported
	 * through the callback before `Create` returns. Errors never contain the client key or the
	 * payload. Returns false, without calling back, when the creator is stopped.
	 *
	 * Creation is idempotent on the server (same content, same link), so timeouts, transport
	 * failures and 5xx are retried with exactly the same body. 4xx, including 429, never are.
	 */
	bool Create(const CreateLinkParams& params, Callback callback);

	/** Number of `Create` calls that have not called back yet. */
	std::size_t InFlightCount() const;

	bool IsStopped() const;

	/** Abandons requests in flight; their callbacks are dropped. `Create` returns false afterwards. */
	void Stop();

private:
	class Impl;
	std::shared_ptr<Impl> impl_;
};

} // namespace strybok
