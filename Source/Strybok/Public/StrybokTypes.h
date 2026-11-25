#pragma once

#include "CoreMinimal.h"
#include "StrybokTypes.generated.h"

class FJsonObject;

/** Why a link could not be resolved, or could not be created. */
UENUM(BlueprintType)
enum class EStrybokErrorKind : uint8
{
	/** ResolveUrl was given a URL that is not a link of this app. No network call was made. */
	UnsupportedLink,
	/** The service does not know this link, or it is disabled or expired (HTTP 404). */
	LinkNotFound,
	/** The request could not be completed (offline, DNS, connection reset) after all retries. */
	Network,
	/** The service did not answer in time, after all retries. */
	Timeout,
	/** The service answered with an unexpected status code or body; see StatusCode. */
	Server,
	/** Unreal only: the subsystem has not been configured yet (or was shut down). */
	NotConfigured,
	/** CreateLink only: no client key is set (no network call), or the service refused it (HTTP 401): wrong or replaced key, or the feature is switched off. */
	Unauthorized,
	/** CreateLink only: the route is not covered by the application's route patterns (HTTP 403); see RoutePath. */
	RouteNotAllowed,
	/** CreateLink only: too many requests or the daily limit is reached (HTTP 429); see RetryAfterSeconds. */
	RateLimited,
	/** CreateLink only: rejected before sending (bad route or payload), or by the service (HTTP 400, 413, 422); see Issues. */
	Validation,
};

/**
 * Where the check for a deferred link (a link tapped before the game was installed) stands. Most
 * installs never involved a link, so finding nothing is the normal result. Nothing about the
 * check is ever reported through OnLinkError.
 */
UENUM(BlueprintType)
enum class EStrybokDeferredLinkStatus : uint8
{
	/** Deferred links are off, or something they need is missing: Android or iOS, the package name / bundle id, or the SDK is not configured. */
	Disabled,
	/** The check is running in this launch. */
	Checking,
	/** Not started yet, or this launch could not finish it (store or network not reachable); a later launch tries again. */
	Pending,
	/** The check was finished in an earlier launch. */
	NotFirstLaunch,
	/** Android: no install referrer source is registered, or none can ever answer. iOS: nothing automatic is switched on (only a pasted link can arrive). */
	Unavailable,
	/** The store returned a referrer without a click id of ours: the usual result of an ordinary install. */
	NoReferrer,
	/** The service refused: unknown, older than allowed, already used, or not this application's. */
	NotFound,
	/** A normal link of this app arrived first. */
	Superseded,
	/** The link was handed to OnLinkReceived (or queued for it). */
	Delivered,
	/** Three launches could not finish the check, or the service's answer was unusable. */
	Failed,
	/** iOS matching only: waiting for RedeemPastedLink or SkipPastedLink, because the pasteboard may hold a link. */
	AwaitingPaste,
};

/** Result of UStrybokSubsystem::RedeemPastedLink. */
UENUM(BlueprintType)
enum class EStrybokPastedLinkResult : uint8
{
	/** The link was handed to OnLinkReceived (or queued for it). */
	Delivered,
	/** Not a link of this app with a click id. Nothing was sent anywhere; the text was dropped. */
	NotOurs,
	/** The service refused the click id: unknown, older than 24 hours, already used, or another app's. */
	NotFound,
	/** A deferred link was already handled for this installation. */
	AlreadyHandled,
	/** Deferred links are off, this is not iOS, the bundle id is missing, or the SDK is not configured. */
	Disabled,
	/** Usually temporary (network, rate limit, server error): the player may paste again. Final only when the status became Failed. */
	Failed,
};

/**
 * Campaign values of an opened link: the usual utm_source, utm_medium, utm_campaign, utm_term and
 * utm_content of web analytics. An empty string means the value is not set.
 *
 * They are the defaults the owner set on the link, overridden by the utm_* parameters of the URL
 * that was tapped. Whoever shares a link can write any value into its URL, so treat them as labels
 * (for your analytics, say), never as something to trust or to act on.
 */
USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokCampaign
{
	GENERATED_BODY()

	/** utm_source, for example "instagram". Empty when not set. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Source;

	/** utm_medium, for example "social". Empty when not set. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Medium;

	/** utm_campaign, for example "spring_sale". Empty when not set. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Campaign;

	/** utm_term. Empty when not set. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Term;

	/** utm_content. Empty when not set. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Content;

	/** True when the link has no campaign value at all. */
	bool IsEmpty() const
	{
		return Source.IsEmpty() && Medium.IsEmpty() && Campaign.IsEmpty() && Term.IsEmpty() && Content.IsEmpty();
	}
};

/**
 * A link resolved by the Strybok API.
 *
 * RoutePath, PayloadJson and Campaign are plain data chosen by whoever created or shared the link.
 * Treat them like any other untrusted input: map them onto levels and screens your game already
 * knows, never execute them (no console commands, no arbitrary level names, no asset paths taken
 * from a link), and use campaign values as labels only.
 */
USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokResolvedLink
{
	GENERATED_BODY()

	/** The full URL that opened the app, exactly as received (including query and fragment). */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Uri;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString LinkId;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString AppId;

	/** In-app route, always starting with a single "/" (for example "/invite/abc123"). */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString RoutePath;

	/** The JSON object attached to the link, as text. "{}" when none was set. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString PayloadJson = TEXT("{}");

	/**
	 * Campaign values of this open: the link's defaults, overridden by the utm_* parameters of the
	 * tapped URL. All five strings are empty when there are none.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FStrybokCampaign Campaign;

	/**
	 * True when the link was restored after the game was installed (deferred links) instead of
	 * arriving from the OS. Then Uri is the link's own URL. False for every other link.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	bool bIsDeferred = false;

	/**
	 * True only for a deferred link found by iOS probabilistic matching. Such a link is a guess
	 * from the network address and MAY BE ANOTHER PERSON'S LINK: never sign anyone in, accept an
	 * invitation, apply a personal discount or show personal content on the strength of it alone.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	bool bIsProbabilisticMatch = false;

	/**
	 * Reads one top-level payload field as text. Works for string fields; numbers and booleans are
	 * converted to text by the engine's JSON module. False when the field is missing, null, an
	 * object or an array.
	 */
	bool GetPayloadString(const FString& Key, FString& OutValue) const;

	/** The payload parsed with the engine's JSON module. Never null; empty when there is no payload. */
	TSharedPtr<FJsonObject> GetPayloadObject() const;

	/** RoutePath split at "/", without empty parts: "/invite/abc123" gives ["invite", "abc123"]. */
	TArray<FString> GetRouteSegments() const;
};

/** One problem the service found in a CreateLink request. */
USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokValidationIssue
{
	GENERATED_BODY()

	/** Which field, for example "title" or "payload.a". May be empty. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Message;
};

USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	EStrybokErrorKind Kind = EStrybokErrorKind::Server;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Message;

	/** The URL that was being handled, when known. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Url;

	/** HTTP status code of the answer when there was one, otherwise 0. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	int32 StatusCode = 0;

	/** RouteNotAllowed: the route that was refused. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString RoutePath;

	/** RateLimited: seconds to wait, from the Retry-After header. -1 when the service did not say. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	int32 RetryAfterSeconds = -1;

	/** Validation from the service: what it found wrong, if it said so. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	TArray<FStrybokValidationIssue> Issues;
};

/**
 * What UStrybokSubsystem::CreateLink sends. Only RoutePath is required; a field left empty is not
 * sent. Do not put personal data in the route or the payload: whoever receives the link can read
 * them.
 */
USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokCreateLinkRequest
{
	GENERATED_BODY()

	/** In-app route the link opens, for example "/product/42". Starts with one "/", at most 512 characters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString RoutePath;

	/** Optional JSON object as text, for example {"productId":42}. Arrays and plain values are rejected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString PayloadJson;

	/** Optional title of the link preview. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString Title;

	/** Optional description of the link preview. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString Description;

	/** Optional image of the link preview; a public https URL. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString ImageUrl;
};

/** A link made by UStrybokSubsystem::CreateLink. */
USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokCreatedLink
{
	GENERATED_BODY()

	/** What the user shares, for example https://my-app.strybok.link/Ab3xK9. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Url;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString Slug;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString RoutePath;

	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	FString LinkId;
};

/** Options of the SDK. The defaults are the ones of the SDK contract. */
USTRUCT(BlueprintType)
struct STRYBOK_API FStrybokConfig
{
	GENERATED_BODY()

	/** Base URL of the Strybok API, e.g. https://api.strybok.com. A path prefix is preserved. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString ApiBaseUrl;

	/** Link hostnames of this app, e.g. my-app.strybok.link. Links on any other host are ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	TArray<FString> AllowedHosts;

	/** Time allowed for a single resolve attempt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok", meta = (ClampMin = "0.1", Units = "s"))
	float TimeoutSeconds = 5.0f;

	/** Extra attempts after a timeout, network error or 5xx. 404 is never retried. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok", meta = (ClampMin = "0", ClampMax = "10"))
	int32 MaxRetries = 2;

	/** Pause before the first retry; doubles for each further retry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok", meta = (ClampMin = "0", Units = "s"))
	float RetryDelaySeconds = 0.4f;

	/** The identical URL received again within this window is a duplicate of the same tap and is dropped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok", meta = (ClampMin = "0", Units = "s"))
	float DedupeWindowSeconds = 2.0f;

	/** Send a best-effort app_open event after a link was resolved. No device identifiers are sent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	bool bReportAppOpens = true;

	/**
	 * Publishable client key of the application ("pk_..."), from the dashboard. Only needed for
	 * CreateLink. It is safe to ship in the game. Never put a secret API key ("lb_...") here.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok")
	FString ClientKey;

	/**
	 * Restore a link that was tapped before the game was installed. Off by default; can also be
	 * switched on later with UStrybokSubsystem::EnableDeferredLinks (for example after a consent
	 * screen). Android: needs AndroidPackageName and an install referrer source registered by the
	 * game. iOS: needs IosBundleId; enables RedeemPastedLink.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok|Deferred links")
	bool bDeferredLinks = false;

	/**
	 * iOS only, together with bDeferredLinks: let the service guess the link from the network
	 * address on first launch. FORBIDDEN BY APPLE'S RULES (it is fingerprinting; the App Review and
	 * legal risk is yours), and a match can be another person's link. Off by default.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok|Deferred links")
	bool bAllowProbabilisticMatching = false;

	/** The game's own Android application ID, e.g. com.example.game. Only used by deferred links. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok|Deferred links")
	FString AndroidPackageName;

	/** The game's own iOS bundle identifier. Only used by deferred links. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Strybok|Deferred links")
	FString IosBundleId;
};

/** What an install referrer source answers (Android deferred links). */
enum class EStrybokInstallReferrerAnswer : uint8
{
	/** Referrer holds the store's raw install referrer string, exactly as returned (it may be empty). */
	Referrer,
	/** The store could not be asked right now; a later launch may succeed. */
	UnavailableForNow,
	/** This store is not on the device, is too old, or its library is not in the game. */
	Unavailable,
};

struct FStrybokInstallReferrerAnswer
{
	EStrybokInstallReferrerAnswer Kind = EStrybokInstallReferrerAnswer::Unavailable;
	FString Referrer;

	static FStrybokInstallReferrerAnswer MakeReferrer(const FString& RawReferrer)
	{
		FStrybokInstallReferrerAnswer Answer;
		Answer.Kind = EStrybokInstallReferrerAnswer::Referrer;
		Answer.Referrer = RawReferrer;
		return Answer;
	}

	static FStrybokInstallReferrerAnswer MakeUnavailableForNow()
	{
		FStrybokInstallReferrerAnswer Answer;
		Answer.Kind = EStrybokInstallReferrerAnswer::UnavailableForNow;
		return Answer;
	}

	static FStrybokInstallReferrerAnswer MakeUnavailable()
	{
		return FStrybokInstallReferrerAnswer();
	}
};

/**
 * One store's install referrer, supplied by the game (the plugin has no store integration of its
 * own). Called on the game thread and must not block it: start the store's asynchronous call and
 * return. Call Complete exactly once, from any thread, with the answer. An answer later than 10
 * seconds is treated as "unavailable for now" and ignored. Must not throw.
 */
using FStrybokInstallReferrerSource = TFunction<void(TFunction<void(const FStrybokInstallReferrerAnswer&)> /*Complete*/)>;

/** The status of the deferred link check changed. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStrybokDeferredLinkStatusChanged, EStrybokDeferredLinkStatus, Status);
DECLARE_MULTICAST_DELEGATE_OneParam(FStrybokDeferredLinkStatusChangedNative, EStrybokDeferredLinkStatus /*Status*/);

/** Result of UStrybokSubsystem::RedeemPastedLink. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FStrybokPastedLinkCompleted, EStrybokPastedLinkResult, Result);

/** A resolved link, delivered exactly once. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStrybokLinkReceived, const FStrybokResolvedLink&, Link);

/** One of our links arrived from the OS but could not be resolved. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStrybokLinkFailed, const FStrybokError&, Error);

/** Result of UStrybokSubsystem::ResolveUrl. Link is valid when bSuccess, otherwise Error is. */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FStrybokResolveCompleted, bool, bSuccess, const FStrybokResolvedLink&, Link, const FStrybokError&, Error);

/** Result of UStrybokSubsystem::CreateLink. Link is valid when bSuccess, otherwise Error is. */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FStrybokCreateLinkCompleted, bool, bSuccess, const FStrybokCreatedLink&, Link, const FStrybokError&, Error);

/** Result of UStrybokSubsystem::GetInitialLink. Link is valid when bHasLink. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FStrybokInitialLinkCompleted, bool, bHasLink, const FStrybokResolvedLink&, Link);

/** C++ flavours of the two multicast delegates above (lambdas, raw and shared pointers). */
DECLARE_MULTICAST_DELEGATE_OneParam(FStrybokLinkReceivedNative, const FStrybokResolvedLink& /*Link*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FStrybokLinkFailedNative, const FStrybokError& /*Error*/);
