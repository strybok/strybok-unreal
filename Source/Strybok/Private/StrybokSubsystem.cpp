#include "StrybokSubsystem.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "StrybokLog.h"
#include "StrybokPlatformLinks.h"
#include "StrybokRuntime.h"
#include "StrybokSettings.h"

void UStrybokSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// One tick per frame: moves URLs from native code to the core and notices new listeners.
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UStrybokSubsystem::Tick));

	const UStrybokSettings* Settings = GetDefault<UStrybokSettings>();
	if (Settings != nullptr && Settings->bAutoConfigure)
	{
		if (Settings->LinkHost.TrimStartAndEnd().IsEmpty() && Settings->AdditionalAllowedHosts.Num() == 0)
		{
			UE_LOG(LogStrybok, Log, TEXT("Strybok is not configured: set Link Host in Project Settings > Plugins > Strybok, or call Configure."));
		}
		else
		{
			ConfigureFromSettings();
		}
	}
}

void UStrybokSubsystem::Deinitialize()
{
	FTSTicker::RemoveTicker(TickHandle);
	TickHandle.Reset();
	Shutdown();
	InitialLinkRequests.Reset();
	Super::Deinitialize();
}

UStrybokSubsystem* UStrybokSubsystem::Get(const UObject* WorldContextObject)
{
	if (GEngine == nullptr || WorldContextObject == nullptr)
	{
		return nullptr;
	}
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	UGameInstance* GameInstance = World != nullptr ? World->GetGameInstance() : nullptr;
	return GameInstance != nullptr ? GameInstance->GetSubsystem<UStrybokSubsystem>() : nullptr;
}

bool UStrybokSubsystem::ConfigureFromSettings()
{
	const UStrybokSettings* Settings = GetDefault<UStrybokSettings>();
	return Settings != nullptr && Configure(Settings->ToConfig());
}

bool UStrybokSubsystem::Configure(const FStrybokConfig& Config)
{
	check(IsInGameThread());

	strybok::Config CoreConfig = StrybokConvert::ToCoreConfig(Config);
	const std::string Problem = strybok::ValidateConfig(CoreConfig);
	if (!Problem.empty())
	{
		UE_LOG(LogStrybok, Error, TEXT("Strybok configuration rejected: %s"), *StrybokConvert::ToFString(Problem));
		return false;
	}

	// Deferred links (optional). Everything the core cannot know comes from the game: the install
	// referrer sources, the device family, the locale and the pasteboard hint. The providers are
	// looked up when the core asks, so they may be registered after this call. The core uses none
	// of it, and never touches the store, while deferred links are off.
	const TSharedRef<FStrybokDeferredProviders> Providers = GetDeferredProviders();
	CoreConfig.deferredLinks = CoreConfig.deferredLinks || bDeferredLinksEnabled;
	CoreConfig.deferredLinkStore = std::make_shared<FStrybokDeferredFileStore>();
	for (const FStrybokInstallReferrerSource& Source : Providers->Sources)
	{
		CoreConfig.installReferrerSources.push_back(StrybokConvert::ToCoreSource(Source));
	}
	CoreConfig.deviceFamily = [Providers]() -> std::string
	{
		return Providers->DeviceFamily ? StrybokConvert::ToStd(Providers->DeviceFamily().TrimStartAndEnd().ToLower()) : std::string();
	};
	CoreConfig.locale = [Providers]() -> std::string
	{
		return Providers->Locale ? StrybokConvert::ToStd(Providers->Locale().TrimStartAndEnd()) : std::string();
	};
	CoreConfig.pasteboardMayContainLink = [Providers](std::function<void(bool)> Answer)
	{
		if (!Providers->PasteboardMayContainLink)
		{
			Answer(false); // no hint from the game: this step is skipped
			return;
		}
		// Boxed: engine closures are relocated with memcpy, which std::function does not survive.
		const TSharedRef<std::function<void(bool)>, ESPMode::ThreadSafe> BoxedAnswer =
			MakeShared<std::function<void(bool)>, ESPMode::ThreadSafe>(std::move(Answer));
		const TFunction<void(TFunction<void(bool)>)> Provider = Providers->PasteboardMayContainLink;
		Provider([BoxedAnswer](bool bMayContainLink)
		{
			if (IsInGameThread())
			{
				(*BoxedAnswer)(bMayContainLink);
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, [BoxedAnswer, bMayContainLink]()
				{
					(*BoxedAnswer)(bMayContainLink);
				});
			}
		});
	};

	// Replace a previous configuration. The old runtime stays alive until the end of this
	// function, in case we are being called from one of its callbacks.
	const TSharedPtr<FStrybokRuntime> Previous = Runtime;
	Runtime.Reset();
	if (Previous.IsValid() && Previous->Engine.IsValid())
	{
		Previous->Engine->Stop();
	}

	const TSharedRef<FStrybokRuntime> NewRuntime = MakeShared<FStrybokRuntime>();
	NewRuntime->Engine = MakeUnique<strybok::LinkEngine>(CoreConfig, NewRuntime->Transport, NewRuntime->Scheduler, NewRuntime->Clock);
	strybok::LinkEngine& Engine = *NewRuntime->Engine;

	const TWeakObjectPtr<UStrybokSubsystem> WeakThis(this);
	Engine.SetLinkHandler([WeakThis](const strybok::ResolvedLink& Link)
	{
		if (UStrybokSubsystem* Self = WeakThis.Get())
		{
			Self->DeliverLink(StrybokConvert::ToLink(Link));
		}
	});
	Engine.SetErrorHandler([WeakThis](const strybok::LinkError& Error)
	{
		if (UStrybokSubsystem* Self = WeakThis.Get())
		{
			Self->DeliverError(StrybokConvert::ToError(Error));
		}
	});
	// Asked right before every delivery, so a link is never broadcast to nobody.
	Engine.SetListeningProbe([WeakThis]() -> bool
	{
		const UStrybokSubsystem* Self = WeakThis.Get();
		return Self != nullptr && Self->HasLinkListener();
	});
	Engine.SetListening(HasLinkListener());
	Engine.SetDeferredLinkStatusHandler([WeakThis](strybok::DeferredLinkStatus Status)
	{
		if (UStrybokSubsystem* Self = WeakThis.Get())
		{
			Self->DeliverDeferredLinkStatus(StrybokConvert::ToStatus(Status));
		}
	});

	Runtime = NewRuntime;

	// CreateLink follows the new options from its next call on.
	LastConfig = Config;
	bHasLastConfig = true;
	ResetCreateRuntime();

	// URLs that native code (or HandleUrl) collected so far. The oldest one is the launch link,
	// but only for the first configuration in this process.
	const TArray<FString> Buffered = FStrybokPlatformLinks::Drain();
	const bool bFirstConfiguration = FStrybokPlatformLinks::ClaimLaunch();
	int32 NextIndex = 0;
	if (bFirstConfiguration && Buffered.Num() > 0)
	{
		const std::string LaunchUrl = StrybokConvert::ToStd(Buffered[0]);
		Engine.Start(&LaunchUrl);
		NextIndex = 1;
	}
	else
	{
		Engine.Start(nullptr);
	}
	for (int32 Index = NextIndex; Index < Buffered.Num(); ++Index)
	{
		if (Runtime.Get() != &NewRuntime.Get() || Engine.IsStopped())
		{
			break; // reconfigured or shut down from a callback
		}
		Engine.HandleUrl(StrybokConvert::ToStd(Buffered[Index]));
	}

	// GetInitialLink calls that came before the configuration.
	TArray<FStrybokInitialLinkCompleted> Requests = MoveTemp(InitialLinkRequests);
	InitialLinkRequests.Reset();
	for (const FStrybokInitialLinkCompleted& Request : Requests)
	{
		GetInitialLink(Request);
	}

	UE_LOG(LogStrybok, Log, TEXT("Strybok configured (%d allowed host(s), launch URL received: %s)."),
		static_cast<int32>(CoreConfig.allowedHosts.size()),
		NextIndex == 1 ? TEXT("yes") : TEXT("no"));
	return true;
}

bool UStrybokSubsystem::IsConfigured() const
{
	return Runtime.IsValid() && Runtime->Engine.IsValid() && !Runtime->Engine->IsStopped();
}

bool UStrybokSubsystem::IsReady() const
{
	return IsConfigured() && Runtime->Engine->IsReady();
}

void UStrybokSubsystem::GetInitialLink(const FStrybokInitialLinkCompleted& OnComplete)
{
	check(IsInGameThread());
	if (!IsConfigured())
	{
		// Answered once Configure has handled the launch link.
		InitialLinkRequests.Add(OnComplete);
		return;
	}
	const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
	const FStrybokInitialLinkCompleted Callback = OnComplete;
	KeepAlive->Engine->GetInitialLink([Callback](const strybok::ResolvedLink* Link)
	{
		Callback.ExecuteIfBound(Link != nullptr, Link != nullptr ? StrybokConvert::ToLink(*Link) : FStrybokResolvedLink());
	});
}

bool UStrybokSubsystem::TryGetInitialLink(FStrybokResolvedLink& OutLink)
{
	check(IsInGameThread());
	if (!IsConfigured())
	{
		return false;
	}
	strybok::ResolvedLink Link;
	if (!Runtime->Engine->TakeInitialLink(Link))
	{
		return false;
	}
	OutLink = StrybokConvert::ToLink(Link);
	return true;
}

void UStrybokSubsystem::ResolveUrl(const FString& Url, const FStrybokResolveCompleted& OnComplete)
{
	check(IsInGameThread());
	const FString Trimmed = Url.TrimStartAndEnd();
	if (!IsConfigured())
	{
		FStrybokError Error;
		Error.Kind = EStrybokErrorKind::NotConfigured;
		Error.Message = TEXT("Strybok is not configured");
		Error.Url = Trimmed;
		OnComplete.ExecuteIfBound(false, FStrybokResolvedLink(), Error);
		return;
	}
	const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
	const FStrybokResolveCompleted Callback = OnComplete;
	KeepAlive->Engine->Resolve(StrybokConvert::ToStd(Trimmed), [Callback](const strybok::ResolvedLink* Link, const strybok::LinkError* Error)
	{
		if (Link != nullptr)
		{
			Callback.ExecuteIfBound(true, StrybokConvert::ToLink(*Link), FStrybokError());
		}
		else if (Error != nullptr)
		{
			Callback.ExecuteIfBound(false, FStrybokResolvedLink(), StrybokConvert::ToError(*Error));
		}
	});
}

void UStrybokSubsystem::HandleUrl(const FString& Url)
{
	check(IsInGameThread());
	const FString Trimmed = Url.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return;
	}
	if (!IsConfigured())
	{
		// Kept with the URLs from native code and processed by Configure.
		FStrybokPlatformLinks::Push(StrybokConvert::ToStd(Trimmed));
		return;
	}
	const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
	KeepAlive->Engine->HandleUrl(StrybokConvert::ToStd(Trimmed));
}

bool UStrybokSubsystem::IsStrybokUrl(const FString& Url) const
{
	return IsConfigured() && Runtime->Engine->IsStrybokUrl(StrybokConvert::ToStd(Url.TrimStartAndEnd()));
}

void UStrybokSubsystem::Shutdown()
{
	// Keep the runtime alive until the end of this function: Shutdown may be called from inside
	// one of its callbacks (for example from an OnLinkReceived handler).
	const TSharedPtr<FStrybokRuntime> Previous = Runtime;
	Runtime.Reset();
	if (Previous.IsValid() && Previous->Engine.IsValid())
	{
		Previous->Engine->Stop();
	}
	ResetCreateRuntime();
}

void UStrybokSubsystem::CreateLink(const FStrybokCreateLinkRequest& Request, const FStrybokCreateLinkCompleted& OnComplete)
{
	const FStrybokCreateLinkCompleted Callback = OnComplete;
	CreateLinkNative(Request, [Callback](bool bSuccess, const FStrybokCreatedLink& Link, const FStrybokError& Error)
	{
		Callback.ExecuteIfBound(bSuccess, Link, Error);
	});
}

void UStrybokSubsystem::CreateLinkNative(
	const FStrybokCreateLinkRequest& Request,
	TFunction<void(bool, const FStrybokCreatedLink&, const FStrybokError&)> OnComplete)
{
	check(IsInGameThread());

	FString Problem;
	// Kept alive until the end of this function: the callback may run before Create returns and
	// may call Shutdown or Configure.
	const TSharedPtr<FStrybokCreateRuntime> KeepAlive = EnsureCreateRuntime(Problem);
	if (!KeepAlive.IsValid())
	{
		UE_LOG(LogStrybok, Warning, TEXT("Strybok cannot create a link: %s"), *Problem);
		if (OnComplete)
		{
			FStrybokError Error;
			Error.Kind = EStrybokErrorKind::NotConfigured;
			Error.Message = Problem;
			OnComplete(false, FStrybokCreatedLink(), Error);
		}
		return;
	}

	KeepAlive->Creator->Create(
		StrybokConvert::ToCoreParams(Request),
		[OnComplete](const strybok::CreatedLink* Link, const strybok::LinkError* Error)
		{
			if (Link != nullptr)
			{
				UE_LOG(LogStrybok, Verbose, TEXT("Strybok created a link."));
				if (OnComplete)
				{
					OnComplete(true, StrybokConvert::ToCreatedLink(*Link), FStrybokError());
				}
			}
			else if (Error != nullptr)
			{
				// Neither the client key nor the payload is part of an error, so this is safe to log.
				const FStrybokError Converted = StrybokConvert::ToError(*Error);
				UE_LOG(LogStrybok, Warning, TEXT("Strybok could not create a link: %s (status %d)"), *Converted.Message, Converted.StatusCode);
				if (OnComplete)
				{
					OnComplete(false, FStrybokCreatedLink(), Converted);
				}
			}
		});
}

TSharedPtr<FStrybokCreateRuntime> UStrybokSubsystem::EnsureCreateRuntime(FString& OutProblem)
{
	if (CreateRuntime.IsValid() && CreateRuntime->Creator.IsValid() && !CreateRuntime->Creator->IsStopped())
	{
		return CreateRuntime;
	}

	FStrybokConfig Config;
	if (bHasLastConfig)
	{
		Config = LastConfig;
	}
	else
	{
		const UStrybokSettings* Settings = GetDefault<UStrybokSettings>();
		if (Settings == nullptr)
		{
			OutProblem = TEXT("Strybok is not configured");
			return nullptr;
		}
		Config = Settings->ToConfig();
	}

	// Only what creating links needs is checked: no link host is required. A missing client key is
	// reported by the core as Unauthorized, per call.
	const strybok::Config CoreConfig = StrybokConvert::ToCoreConfig(Config);
	const std::string Problem = strybok::ValidateCreateLinkConfig(CoreConfig);
	if (!Problem.empty())
	{
		OutProblem = StrybokConvert::ToFString(Problem);
		return nullptr;
	}

	const TSharedRef<FStrybokCreateRuntime> NewRuntime = MakeShared<FStrybokCreateRuntime>();
	NewRuntime->Creator = MakeUnique<strybok::LinkCreator>(CoreConfig, NewRuntime->Transport, NewRuntime->Scheduler);
	CreateRuntime = NewRuntime;
	return CreateRuntime;
}

void UStrybokSubsystem::ResetCreateRuntime()
{
	// Kept alive until the end of this function: we may be inside one of its callbacks.
	const TSharedPtr<FStrybokCreateRuntime> Previous = CreateRuntime;
	CreateRuntime.Reset();
	if (Previous.IsValid() && Previous->Creator.IsValid())
	{
		Previous->Creator->Stop();
	}
}

bool UStrybokSubsystem::Tick(float /*DeltaSeconds*/)
{
	if (IsConfigured())
	{
		const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
		strybok::LinkEngine& Engine = *KeepAlive->Engine;

		// Links received by native code since the last frame.
		const TArray<FString> Urls = FStrybokPlatformLinks::Drain();
		for (const FString& Url : Urls)
		{
			if (Engine.IsStopped())
			{
				break;
			}
			Engine.HandleUrl(StrybokConvert::ToStd(Url));
		}

		// Dynamic delegates cannot tell us when something binds, so look once per frame. Turning
		// listening on delivers the links that were queued while nothing was bound.
		if (!Engine.IsStopped())
		{
			Engine.SetListening(HasLinkListener());
		}
	}
	return true; // keep ticking
}

bool UStrybokSubsystem::HasLinkListener() const
{
	return OnLinkReceived.IsBound() || LinkReceivedNative.IsBound();
}

void UStrybokSubsystem::DeliverLink(const FStrybokResolvedLink& Link)
{
	UE_LOG(LogStrybok, Verbose, TEXT("Strybok link %s -> %s"), *Link.LinkId, *Link.RoutePath);
	OnLinkReceived.Broadcast(Link);
	LinkReceivedNative.Broadcast(Link);
}

// ---------------------------------------------------------------------------------------------
// Deferred links
// ---------------------------------------------------------------------------------------------

TSharedRef<FStrybokDeferredProviders> UStrybokSubsystem::GetDeferredProviders()
{
	if (!DeferredProviders.IsValid())
	{
		DeferredProviders = MakeShared<FStrybokDeferredProviders>();
	}
	return DeferredProviders.ToSharedRef();
}

EStrybokDeferredLinkStatus UStrybokSubsystem::GetDeferredLinkStatus() const
{
	return IsConfigured() ? StrybokConvert::ToStatus(Runtime->Engine->GetDeferredLinkStatus()) : EStrybokDeferredLinkStatus::Disabled;
}

void UStrybokSubsystem::EnableDeferredLinks()
{
	check(IsInGameThread());
	bDeferredLinksEnabled = true; // also for a later Configure
	if (IsConfigured())
	{
		const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
		KeepAlive->Engine->EnableDeferredLinks();
	}
}

void UStrybokSubsystem::RedeemPastedLink(const FString& PastedText, const FStrybokPastedLinkCompleted& OnComplete)
{
	const FStrybokPastedLinkCompleted Callback = OnComplete;
	RedeemPastedLinkNative(PastedText, [Callback](EStrybokPastedLinkResult Result)
	{
		Callback.ExecuteIfBound(Result);
	});
}

void UStrybokSubsystem::RedeemPastedLinkNative(const FString& PastedText, TFunction<void(EStrybokPastedLinkResult)> OnComplete)
{
	check(IsInGameThread());
	if (!IsConfigured())
	{
		if (OnComplete)
		{
			OnComplete(EStrybokPastedLinkResult::Disabled);
		}
		return;
	}
	// The text goes to the core as it is and is never logged: it is whatever the player had on
	// the clipboard.
	const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
	KeepAlive->Engine->RedeemPastedLink(StrybokConvert::ToStd(PastedText), [OnComplete](strybok::PastedLinkResult Result)
	{
		if (OnComplete)
		{
			OnComplete(StrybokConvert::ToPastedResult(Result));
		}
	});
}

void UStrybokSubsystem::SkipPastedLink()
{
	check(IsInGameThread());
	if (IsConfigured())
	{
		const TSharedPtr<FStrybokRuntime> KeepAlive = Runtime;
		KeepAlive->Engine->SkipPastedLink();
	}
}

void UStrybokSubsystem::AddInstallReferrerSource(FStrybokInstallReferrerSource Source)
{
	check(IsInGameThread());
	if (!Source)
	{
		return;
	}
	const TSharedRef<FStrybokDeferredProviders> Providers = GetDeferredProviders();
	Providers->Sources.push_back(MoveTemp(Source));
	if (IsConfigured())
	{
		// The core takes the list when the check starts asking; after that this changes nothing.
		std::vector<strybok::InstallReferrerSource> Sources;
		for (const FStrybokInstallReferrerSource& Registered : Providers->Sources)
		{
			Sources.push_back(StrybokConvert::ToCoreSource(Registered));
		}
		Runtime->Engine->SetInstallReferrerSources(std::move(Sources));
	}
}

void UStrybokSubsystem::SetDeviceFamilyProvider(TFunction<FString()> Provider)
{
	check(IsInGameThread());
	GetDeferredProviders()->DeviceFamily = MoveTemp(Provider);
}

void UStrybokSubsystem::SetLocaleProvider(TFunction<FString()> Provider)
{
	check(IsInGameThread());
	GetDeferredProviders()->Locale = MoveTemp(Provider);
}

void UStrybokSubsystem::SetPasteboardMayContainLinkProvider(TFunction<void(TFunction<void(bool)>)> Provider)
{
	check(IsInGameThread());
	GetDeferredProviders()->PasteboardMayContainLink = MoveTemp(Provider);
}

void UStrybokSubsystem::DeliverDeferredLinkStatus(EStrybokDeferredLinkStatus Status)
{
	// Only the status is logged, never a referrer, a pasted text or a click id.
	UE_LOG(LogStrybok, Verbose, TEXT("Strybok deferred link status: %d"), static_cast<int32>(Status));
	OnDeferredLinkStatusChanged.Broadcast(Status);
	DeferredLinkStatusChangedNative.Broadcast(Status);
}

void UStrybokSubsystem::DeliverError(const FStrybokError& Error)
{
	// The URL is not logged: its query may hold private data.
	UE_LOG(LogStrybok, Warning, TEXT("Strybok could not resolve a link: %s (status %d)"), *Error.Message, Error.StatusCode);
	OnLinkError.Broadcast(Error);
	LinkErrorNative.Broadcast(Error);
}
