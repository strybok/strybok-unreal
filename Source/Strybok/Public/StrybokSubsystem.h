#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StrybokTypes.h"
#include "StrybokSubsystem.generated.h"

struct FStrybokRuntime;
struct FStrybokCreateRuntime;
struct FStrybokDeferredProviders;

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
	 * Asks the service for a short link to a screen of this game (a "Share" button). OnComplete
	 * gets the link (share Link.Url) or an error; errors of CreateLink never fire OnLinkError.
	 *
	 * Needs the publishable Client Key ("pk_...") from the dashboard, where creating links from the
	 * app must be switched on and the allowed route patterns listed. Never use a secret API key.
	 *
	 * It only runs when you call it and has nothing to do with receiving links: it works before
	 * and without Configure (then with Project Settings > Plugins > Strybok) and needs no link
	 * host. After a successful Configure it uses that configuration.
	 *
	 * Errors: Unauthorized (no key set: no network call; or the key was refused), Validation (bad
	 * route or payload: no network call; or rejected by the service, see Issues), RouteNotAllowed,
	 * RateLimited (see RetryAfterSeconds), Network / Timeout / Server after the retries, and
	 * NotConfigured when the API base URL is unusable. RateLimited, Network, Timeout and Server
	 * are temporary: let the player try again later. The others need a change in the game or in
	 * the dashboard.
	 *
	 * Asking again for the same content returns the same link. Shutdown, a new Configure and the
	 * end of the game instance abandon requests in flight: their OnComplete is not called.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	void CreateLink(const FStrybokCreateLinkRequest& Request, const FStrybokCreateLinkCompleted& OnComplete);

	/** CreateLink for C++ lambdas. Link is valid when bSuccess, otherwise Error is. */
	void CreateLinkNative(
		const FStrybokCreateLinkRequest& Request,
		TFunction<void(bool /*bSuccess*/, const FStrybokCreatedLink& /*Link*/, const FStrybokError& /*Error*/)> OnComplete);

	/**
	 * Stops the SDK: nothing is delivered afterwards, including links that were still resolving,
	 * and CreateLink requests in flight are abandoned. Happens automatically when the game
	 * instance shuts down. Configure starts it again.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok")
	void Shutdown();

	// --- Deferred links (optional, off by default): a link tapped before the game was installed ---
	//
	// The plugin has no store integration and never reads the pasteboard. Android: the game
	// registers one or more install referrer sources (AddInstallReferrerSource, C++ only) and sets
	// the package name. iOS: the game hands over what the player pasted (RedeemPastedLink) and
	// sets the bundle id. A deferred link arrives through OnLinkReceived like any other, with
	// bIsDeferred set; GetInitialLink never returns it and no app_open event is sent for it.
	// Nothing here ever fires OnLinkError.

	/** Where the deferred link check stands. Disabled while the SDK is not configured. */
	UFUNCTION(BlueprintPure, Category = "Strybok|Deferred links")
	EStrybokDeferredLinkStatus GetDeferredLinkStatus() const;

	/** Fires on the game thread whenever GetDeferredLinkStatus changes. For diagnostics and QA; do not show it to players. */
	UPROPERTY(BlueprintAssignable, Category = "Strybok|Deferred links")
	FStrybokDeferredLinkStatusChanged OnDeferredLinkStatusChanged;

	/** Same as OnDeferredLinkStatusChanged, for C++ lambdas. */
	FStrybokDeferredLinkStatusChangedNative& OnDeferredLinkStatusChangedNative() { return DeferredLinkStatusChangedNative; }

	/**
	 * Switches deferred links on after start-up, for example after a consent screen, and runs the
	 * check once (once the launch link has been handled; never more than once per configuration).
	 * Called before Configure it is remembered. Calling it again does nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok|Deferred links")
	void EnableDeferredLinks();

	/**
	 * iOS: hands over the text the player pasted with a paste control or paste action of the game.
	 * If it is a link of this app with a click id, the id is exchanged for the link once and the
	 * link is delivered through OnLinkReceived (even if a normal link was delivered before).
	 * Anything else gives NotOurs: nothing is sent, stored or logged. OnComplete is not called
	 * when the SDK is shut down or reconfigured while the request is in flight.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok|Deferred links")
	void RedeemPastedLink(const FString& PastedText, const FStrybokPastedLinkCompleted& OnComplete);

	/** RedeemPastedLink for C++ lambdas. */
	void RedeemPastedLinkNative(const FString& PastedText, TFunction<void(EStrybokPastedLinkResult /*Result*/)> OnComplete);

	/**
	 * iOS, only with probabilistic matching and a pasteboard provider: the player moved on without
	 * pasting, so matching may start (status AwaitingPaste). Does nothing otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Strybok|Deferred links")
	void SkipPastedLink();

	/**
	 * Android: adds an install referrer source of the game; sources are asked in the order added.
	 * Game thread only. The list is read at the moment the check starts asking, which is one frame
	 * after the launch link was handled when Deferred Links is on in the settings, or one frame
	 * after EnableDeferredLinks. So either add the sources in the frame the game instance starts
	 * (UGameInstance::Init, after Super::Init), or - simpler and safe - leave Deferred Links off in
	 * the settings and call EnableDeferredLinks after adding them. A source added after the check
	 * began is not asked in this launch. With no source at all the check ends as Unavailable and is
	 * not repeated on later launches.
	 */
	void AddInstallReferrerSource(FStrybokInstallReferrerSource Source);

	/**
	 * iOS probabilistic matching: returns "iphone", "ipad" or "ipod" (from the UIKit idiom and
	 * device model; anything else means "not such a device"). Called on the game thread. Without
	 * it, or without a locale provider, matching ends as Unavailable.
	 */
	void SetDeviceFamilyProvider(TFunction<FString()> Provider);

	/** iOS probabilistic matching: returns the first preferred language as a BCP 47 tag such as "en-GB" (NSLocale.preferredLanguages.firstObject). */
	void SetLocaleProvider(TFunction<FString()> Provider);

	/**
	 * iOS probabilistic matching, optional: answers whether the pasteboard probably holds a web
	 * URL, found out WITHOUT reading it (UIPasteboard detectPatterns with probableWebURL).
	 * Called on the game thread; Complete may be called from any thread. When it answers true,
	 * matching waits for RedeemPastedLink or SkipPastedLink.
	 */
	void SetPasteboardMayContainLinkProvider(TFunction<void(TFunction<void(bool)> /*Complete*/)> Provider);

private:
	bool Tick(float DeltaSeconds);
	bool HasLinkListener() const;
	void DeliverLink(const FStrybokResolvedLink& Link);
	void DeliverError(const FStrybokError& Error);
	void DeliverDeferredLinkStatus(EStrybokDeferredLinkStatus Status);
	TSharedRef<FStrybokDeferredProviders> GetDeferredProviders();

	/** What the game registered for deferred links; made on first use, kept across configurations. */
	TSharedPtr<FStrybokDeferredProviders> DeferredProviders;

	/** EnableDeferredLinks was called: later configurations start with deferred links on. */
	bool bDeferredLinksEnabled = false;

	FStrybokDeferredLinkStatusChangedNative DeferredLinkStatusChangedNative;

	/** The runtime of CreateLink, made on first use. Null with a problem text when the options are unusable. */
	TSharedPtr<FStrybokCreateRuntime> EnsureCreateRuntime(FString& OutProblem);
	void ResetCreateRuntime();

	/** Transport, timers and the engine-independent core. Null while not configured. */
	TSharedPtr<FStrybokRuntime> Runtime;

	/** Transport, timers and the core's link creator. Null until CreateLink is first used. */
	TSharedPtr<FStrybokCreateRuntime> CreateRuntime;

	/** The options of the last successful Configure; CreateLink uses the project settings without one. */
	FStrybokConfig LastConfig;
	bool bHasLastConfig = false;

	FStrybokLinkReceivedNative LinkReceivedNative;
	FStrybokLinkFailedNative LinkErrorNative;

	/** GetInitialLink calls made before Configure. */
	TArray<FStrybokInitialLinkCompleted> InitialLinkRequests;

	FTSTicker::FDelegateHandle TickHandle;
};
