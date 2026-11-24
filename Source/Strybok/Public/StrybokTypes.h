#pragma once

#include "CoreMinimal.h"
#include "StrybokTypes.generated.h"

class FJsonObject;

/** Why a link could not be resolved. */
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

	/** HTTP status code for Server and LinkNotFound errors, otherwise 0. */
	UPROPERTY(BlueprintReadOnly, Category = "Strybok")
	int32 StatusCode = 0;
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
};

/** A resolved link, delivered exactly once. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStrybokLinkReceived, const FStrybokResolvedLink&, Link);

/** One of our links arrived from the OS but could not be resolved. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStrybokLinkFailed, const FStrybokError&, Error);

/** Result of UStrybokSubsystem::ResolveUrl. Link is valid when bSuccess, otherwise Error is. */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FStrybokResolveCompleted, bool, bSuccess, const FStrybokResolvedLink&, Link, const FStrybokError&, Error);

/** Result of UStrybokSubsystem::GetInitialLink. Link is valid when bHasLink. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FStrybokInitialLinkCompleted, bool, bHasLink, const FStrybokResolvedLink&, Link);

/** C++ flavours of the two multicast delegates above (lambdas, raw and shared pointers). */
DECLARE_MULTICAST_DELEGATE_OneParam(FStrybokLinkReceivedNative, const FStrybokResolvedLink& /*Link*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FStrybokLinkFailedNative, const FStrybokError& /*Error*/);
