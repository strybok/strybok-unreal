#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StrybokTypes.h"
#include "StrybokSubsystem.generated.h"

struct FStrybokRuntime;

/**
 * Entry point of the Strybok SDK.
 *
 * The OS opens the app with a link such as https://my-app.strybok.link/invite777. The subsystem
 * asks the Strybok API which in-app route and payload that link stands for and hands your game
 * each link exactly once, on cold start, from the background and in the foreground.
 *
 * Blueprint: Get Game Instance Subsystem (Strybok Subsystem), bind an event to On Link Received.
 *
 * C++:
 *   UStrybokSubsystem* Strybok = GetGameInstance()->GetSubsystem<UStrybokSubsystem>();
 *   Strybok->OnLinkReceived.AddDynamic(this, &UMyGameInstance::HandleLink);
 *
 * With Project Settings > Plugins > Strybok filled in, the subsystem configures itself when the
 * game instance starts. All functions must be called on the game thread; all delegates fire there.
 */
UCLASS()
class STRYBOK_API UStrybokSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// UGameInstanceSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The subsystem of the game instance that owns WorldContextObject, or null. */
	static UStrybokSubsystem* Get(const UObject* WorldContextObject);

	/**
	 * Starts the SDK with explicit options: begins accepting links and resolves the link that
	 * launched the app, if any. Returns false (and logs why) when the options are unusable.
	 *
	 * Calling it again replaces the configuration: links that were still resolving or waiting for a
	 * listener are dropped. The launch link is only ever handled by the first call in a process.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	bool Configure(const FStrybokConfig& Config);

	/** Configure with the values from Project Settings > Plugins > Strybok. */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	bool ConfigureFromSettings();

	/** Whether Configure succeeded and Shutdown was not called since. */
	UFUNCTION(BlueprintPure, Category = "Strybok")
	bool IsConfigured() const;

	/** True once the link that launched the app (if any) has been resolved or has failed. */
	UFUNCTION(BlueprintPure, Category = "Strybok")
	bool IsReady() const;

	/**
	 * Resolved links, each delivered exactly once.
	 *
	 * Links that arrive while nothing is bound (typically the one that cold-started the app) are
	 * kept and delivered to the first binder, on the next frame after it binds. If several objects
	 * are bound, each sees every link once.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Strybok")
	FStrybokLinkReceived OnLinkReceived;

	/**
	 * A link of this app arrived from the OS but could not be resolved (offline, expired link, ...).
	 * Later links keep working. URLs that are simply not ours never end up here.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Strybok")
	FStrybokLinkFailed OnLinkError;

	/** Same stream as OnLinkReceived, for C++ lambdas. A link goes to both delegates, once each. */
	FStrybokLinkReceivedNative& OnLinkReceivedNative() { return LinkReceivedNative; }

	/** Same stream as OnLinkError, for C++ lambdas. */
	FStrybokLinkFailedNative& OnLinkErrorNative() { return LinkErrorNative; }

	/**
	 * The link that launched the app, if it has not been delivered through OnLinkReceived yet.
	 * Completes once the launch link has been resolved (or has failed). Hands the link out at most
	 * once and removes it from OnLinkReceived, so using both never navigates twice.
	 *
	 * Note: if something is bound to OnLinkReceived, it usually receives the launch link first and
	 * this completes with bHasLink = false. Use one of the two styles for start-up navigation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	void GetInitialLink(const FStrybokInitialLinkCompleted& OnComplete);

	/**
	 * Synchronous variant of GetInitialLink: false when the launch link is not resolved yet, when
	 * there is none, or when it was already delivered.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	bool TryGetInitialLink(FStrybokResolvedLink& OutLink);

	/**
	 * Resolves a URL explicitly (QR scanner, in-game inbox, tests). Does not fire OnLinkReceived
	 * and does not report an app open. Fails with UnsupportedLink, without a network call, when the
	 * URL is not a link of this app.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	void ResolveUrl(const FString& Url, const FStrybokResolveCompleted& OnComplete);

	/**
	 * Manual entry point: feeds a URL into the same flow as a link received from the OS
	 * (validation, duplicate protection, resolve, OnLinkReceived / OnLinkError). Use it when your
	 * project receives URLs through its own native code, or to simulate links in the editor.
	 * URLs handed over before Configure are kept and processed once the SDK is configured.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	void HandleUrl(const FString& Url);

	/**
	 * Whether Url looks like a Strybok link of this app: https, a host from the allowed hosts,
	 * default port, no credentials and a single slug path. False while not configured.
	 */
	UFUNCTION(BlueprintPure, Category = "Strybok")
	bool IsStrybokUrl(const FString& Url) const;

	/**
	 * Stops the SDK: nothing is delivered afterwards, including links that were still resolving.
	 * Happens automatically when the game instance shuts down. Configure starts it again.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	void Shutdown();

private:
	bool Tick(float DeltaSeconds);
	bool HasLinkListener() const;
	void DeliverLink(const FStrybokResolvedLink& Link);
	void DeliverError(const FStrybokError& Error);

	/** Transport, timers and the engine-independent core. Null while not configured. */
	TSharedPtr<FStrybokRuntime> Runtime;

	FStrybokLinkReceivedNative LinkReceivedNative;
	FStrybokLinkFailedNative LinkErrorNative;

	/** GetInitialLink calls made before Configure. */
	TArray<FStrybokInitialLinkCompleted> InitialLinkRequests;

	FTSTicker::FDelegateHandle TickHandle;
};
