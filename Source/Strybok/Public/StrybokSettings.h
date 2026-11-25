#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "StrybokTypes.h"
#include "StrybokSettings.generated.h"

/**
 * Project Settings > Plugins > Strybok. Stored in Config/DefaultGame.ini:
 *
 *   [/Script/Strybok.StrybokSettings]
 *   ApiBaseUrl=https://api.strybok.com
 *   LinkHost=my-app.strybok.link
 *
 * LinkHost is also read at packaging time by Strybok_UPL.xml to add the Android App Links intent
 * filter, so keep it in DefaultGame.ini (not in a platform or user override).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Strybok"))
class STRYBOK_API UStrybokSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UStrybokSettings();

	virtual FName GetCategoryName() const override;

	/**
	 * Configure the subsystem from these settings when the game instance starts. Turn off to call
	 * Configure yourself (for example with a different API per build flavour).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "General")
	bool bAutoConfigure = true;

	/** Base URL of the Strybok API. */
	UPROPERTY(Config, EditAnywhere, Category = "General")
	FString ApiBaseUrl = TEXT("https://api.strybok.com");

	/**
	 * The managed link hostname of your application, for example my-app.strybok.link (no scheme,
	 * no path). Used as an allowed host and for the Android intent filter.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "General")
	FString LinkHost;

	/**
	 * More hostnames whose links this app accepts (for example a custom domain). The Android intent
	 * filter and the iOS Associated Domains entry for these must be added by hand.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "General")
	TArray<FString> AdditionalAllowedHosts;

	/** Time allowed for a single resolve attempt. */
	UPROPERTY(Config, EditAnywhere, Category = "Network", meta = (ClampMin = "0.1", Units = "s"))
	float TimeoutSeconds = 5.0f;

	/** Extra attempts after a timeout, network error or 5xx. */
	UPROPERTY(Config, EditAnywhere, Category = "Network", meta = (ClampMin = "0", ClampMax = "10"))
	int32 MaxRetries = 2;

	/** Pause before the first retry; doubles for each further retry. */
	UPROPERTY(Config, EditAnywhere, Category = "Network", meta = (ClampMin = "0", Units = "s"))
	float RetryDelaySeconds = 0.4f;

	/** The identical URL received again within this window is dropped as a duplicate. */
	UPROPERTY(Config, EditAnywhere, Category = "Delivery", meta = (ClampMin = "0", Units = "s"))
	float DedupeWindowSeconds = 2.0f;

	/** Send a best-effort app_open event after a link was resolved. No device identifiers are sent. */
	UPROPERTY(Config, EditAnywhere, Category = "Analytics")
	bool bReportAppOpens = true;

	/**
	 * Publishable client key of the application ("pk_..."), from the dashboard ("Links from the
	 * app"). Only needed for CreateLink. It is safe to ship in the game: it can only create links
	 * for the route patterns listed in the dashboard. Never put a secret API key ("lb_...") here.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Link creation")
	FString ClientKey;

	/**
	 * Restore a link that was tapped before the game was installed. Off by default. With it on, the
	 * check runs right after start-up; leave it off and call UStrybokSubsystem::EnableDeferredLinks
	 * instead when the player has to consent first, or when your install referrer sources are
	 * registered later than the first frame. See the integration guide: on Android the game must
	 * supply the install referrer itself, on iOS the player must paste the link.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Deferred links", meta = (DisplayName = "Deferred Links"))
	bool bDeferredLinks = false;

	/**
	 * iOS only, together with Deferred Links: let the service guess the link from the network
	 * address. FORBIDDEN BY APPLE'S RULES (fingerprinting); the App Review and legal risk is yours,
	 * and a match can be another person's link. Off by default.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Deferred links", meta = (DisplayName = "Allow Probabilistic Matching (iOS, forbidden by Apple)"))
	bool bAllowProbabilisticMatching = false;

	/** The game's own Android application ID (the "Android Package Name" of the Android platform settings). Deferred links on Android stay disabled without it. */
	UPROPERTY(Config, EditAnywhere, Category = "Deferred links")
	FString AndroidPackageName;

	/** The game's own iOS bundle identifier. Deferred links on iOS stay disabled without it. */
	UPROPERTY(Config, EditAnywhere, Category = "Deferred links")
	FString IosBundleId;

	/** These settings as a config for UStrybokSubsystem::Configure. */
	FStrybokConfig ToConfig() const;
};
