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
 * A link resolved by the Strybok API.
 *
 * RoutePath and PayloadJson are plain data chosen by whoever created the link. Treat them like any
 * other untrusted input: map them onto levels and screens your game already knows, never execute
 * them (no console commands, no arbitrary level names, no asset paths taken from a link).
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
};

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
