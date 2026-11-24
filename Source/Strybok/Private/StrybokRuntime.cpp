#include "StrybokRuntime.h"

#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"

// ---------------------------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------------------------

std::string StrybokConvert::ToStd(const FString& Value)
{
	return std::string(TCHAR_TO_UTF8(*Value));
}

FString StrybokConvert::ToFString(const std::string& Value)
{
	return FString(UTF8_TO_TCHAR(Value.c_str()));
}

FStrybokResolvedLink StrybokConvert::ToLink(const strybok::ResolvedLink& Link)
{
	FStrybokResolvedLink Result;
	Result.Uri = ToFString(Link.uri);
	Result.LinkId = ToFString(Link.linkId);
	Result.AppId = ToFString(Link.appId);
	Result.RoutePath = ToFString(Link.routePath);
	Result.PayloadJson = ToFString(Link.payloadJson);
	return Result;
}

FStrybokError StrybokConvert::ToError(const strybok::LinkError& Error)
{
	FStrybokError Result;
	switch (Error.kind)
	{
	case strybok::ErrorKind::UnsupportedLink:
		Result.Kind = EStrybokErrorKind::UnsupportedLink;
		break;
	case strybok::ErrorKind::LinkNotFound:
		Result.Kind = EStrybokErrorKind::LinkNotFound;
		break;
	case strybok::ErrorKind::Network:
		Result.Kind = EStrybokErrorKind::Network;
		break;
	case strybok::ErrorKind::Timeout:
		Result.Kind = EStrybokErrorKind::Timeout;
		break;
	case strybok::ErrorKind::Unauthorized:
		Result.Kind = EStrybokErrorKind::Unauthorized;
		break;
	case strybok::ErrorKind::RouteNotAllowed:
		Result.Kind = EStrybokErrorKind::RouteNotAllowed;
		break;
	case strybok::ErrorKind::RateLimited:
		Result.Kind = EStrybokErrorKind::RateLimited;
		break;
	case strybok::ErrorKind::Validation:
		Result.Kind = EStrybokErrorKind::Validation;
		break;
	case strybok::ErrorKind::Server:
	default:
		Result.Kind = EStrybokErrorKind::Server;
		break;
	}
	Result.Message = ToFString(Error.message);
	Result.Url = ToFString(Error.url);
	Result.StatusCode = Error.statusCode;
	Result.RoutePath = ToFString(Error.routePath);
	// The core caps the value at the largest int32.
	Result.RetryAfterSeconds = Error.retryAfterSeconds < 0 ? -1 : static_cast<int32>(Error.retryAfterSeconds);
	for (const strybok::ValidationIssue& Issue : Error.issues)
	{
		FStrybokValidationIssue Converted;
		Converted.Path = ToFString(Issue.path);
		Converted.Message = ToFString(Issue.message);
		Result.Issues.Add(MoveTemp(Converted));
	}
	return Result;
}

FStrybokCreatedLink StrybokConvert::ToCreatedLink(const strybok::CreatedLink& Link)
{
	FStrybokCreatedLink Result;
	Result.Url = ToFString(Link.url);
	Result.Slug = ToFString(Link.slug);
	Result.RoutePath = ToFString(Link.routePath);
	Result.LinkId = ToFString(Link.linkId);
	return Result;
}

strybok::CreateLinkParams StrybokConvert::ToCoreParams(const FStrybokCreateLinkRequest& Request)
{
	strybok::CreateLinkParams Result;
	// The route is sent exactly as given: the core rejects one that does not start with "/".
	Result.routePath = ToStd(Request.RoutePath);
	Result.payloadJson = ToStd(Request.PayloadJson.TrimStartAndEnd());
	Result.title = ToStd(Request.Title);
	Result.description = ToStd(Request.Description);
	Result.imageUrl = ToStd(Request.ImageUrl.TrimStartAndEnd());
	return Result;
}

strybok::Config StrybokConvert::ToCoreConfig(const FStrybokConfig& Config)
{
	strybok::Config Result;
	Result.apiBaseUrl = ToStd(Config.ApiBaseUrl.TrimStartAndEnd());
	for (const FString& Host : Config.AllowedHosts)
	{
		Result.allowedHosts.push_back(ToStd(Host));
	}
	Result.timeoutMs = static_cast<std::int64_t>(static_cast<double>(Config.TimeoutSeconds) * 1000.0 + 0.5);
	Result.maxRetries = static_cast<int>(Config.MaxRetries);
	Result.retryDelayMs = static_cast<std::int64_t>(static_cast<double>(Config.RetryDelaySeconds) * 1000.0 + 0.5);
	Result.dedupeWindowMs = static_cast<std::int64_t>(static_cast<double>(Config.DedupeWindowSeconds) * 1000.0 + 0.5);
	Result.reportAppOpens = Config.bReportAppOpens;
#if PLATFORM_IOS
	Result.platform = "ios";
#elif PLATFORM_ANDROID
	Result.platform = "android";
#else
	Result.platform = "unknown";
#endif
	Result.sdkVersion = strybok::kSdkVersion;
	Result.clientKey = ToStd(Config.ClientKey.TrimStartAndEnd());
	return Result;
}

// ---------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------

FStrybokHttpTransport::FStrybokHttpTransport()
	: State(MakeShared<FState>())
{
}

FStrybokHttpTransport::~FStrybokHttpTransport()
{
	std::map<std::uint64_t, FPending> Pending;
	Pending.swap(State->Pending);
	for (std::pair<const std::uint64_t, FPending>& Entry : Pending)
	{
		if (Entry.second.Request.IsValid())
		{
			Entry.second.Request->OnProcessRequestComplete().Unbind();
			Entry.second.Request->CancelRequest();
		}
	}
}

std::uint64_t FStrybokHttpTransport::Send(const strybok::HttpRequest& Request, Callback OnDone)
{
	const std::uint64_t Id = ++NextId;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
	Http->SetVerb(StrybokConvert::ToFString(Request.method));
	Http->SetURL(StrybokConvert::ToFString(Request.url));
	for (const std::pair<std::string, std::string>& Header : Request.headers)
	{
		Http->SetHeader(StrybokConvert::ToFString(Header.first), StrybokConvert::ToFString(Header.second));
	}
	if (!Request.body.empty())
	{
		Http->SetContentAsString(StrybokConvert::ToFString(Request.body));
	}

	// The per-attempt timeout is enforced by the core (it cancels the request), so the engine's
	// own HTTP timeouts only matter when they are shorter; that shows up as a network error.
	const TWeakPtr<FState> WeakState = State;
	Http->OnProcessRequestComplete().BindLambda(
		[WeakState, Id](FHttpRequestPtr /*HttpRequest*/, FHttpResponsePtr HttpResponse, bool bSucceeded)
		{
			strybok::HttpResponse Response;
			if (bSucceeded && HttpResponse.IsValid())
			{
				Response.transportOk = true;
				Response.statusCode = static_cast<int>(HttpResponse->GetResponseCode());
				Response.body = StrybokConvert::ToStd(HttpResponse->GetContentAsString());
				// Empty when the header is absent. Only used for 429 answers to CreateLink.
				Response.retryAfter = StrybokConvert::ToStd(HttpResponse->GetHeader(TEXT("Retry-After")));
			}
			if (IsInGameThread())
			{
				FStrybokHttpTransport::Complete(WeakState, Id, Response);
			}
			else
			{
				// Not expected with the default delegate thread policy; the core is game thread only.
				const TSharedRef<strybok::HttpResponse> Boxed = MakeShared<strybok::HttpResponse>(Response);
				AsyncTask(ENamedThreads::GameThread, [WeakState, Id, Boxed]()
				{
					FStrybokHttpTransport::Complete(WeakState, Id, *Boxed);
				});
			}
		});

	FPending& Pending = State->Pending[Id];
	Pending.Request = Http;
	Pending.OnDone = std::move(OnDone);

	if (!Http->ProcessRequest())
	{
		// Could not even start. If the delegate already reported it, this finds nothing to do.
		Complete(WeakState, Id, strybok::HttpResponse());
	}
	return Id;
}

void FStrybokHttpTransport::Cancel(std::uint64_t RequestId)
{
	const auto Found = State->Pending.find(RequestId);
	if (Found == State->Pending.end())
	{
		return;
	}
	const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = Found->second.Request;
	State->Pending.erase(Found);
	if (Request.IsValid())
	{
		// Unbind first: CancelRequest reports completion, and the core must not hear about it.
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
	}
}

void FStrybokHttpTransport::Complete(const TWeakPtr<FState>& WeakState, std::uint64_t Id, const strybok::HttpResponse& Response)
{
	const TSharedPtr<FState> Pinned = WeakState.Pin();
	if (!Pinned.IsValid())
	{
		return;
	}
	const auto Found = Pinned->Pending.find(Id);
	if (Found == Pinned->Pending.end())
	{
		return; // cancelled, or already completed
	}
	// Take the callback out before calling it: it may send or cancel other requests.
	const Callback OnDone = std::move(Found->second.OnDone);
	Pinned->Pending.erase(Found);
	if (OnDone)
	{
		OnDone(Response);
	}
}

// ---------------------------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------------------------

FStrybokTickerScheduler::FStrybokTickerScheduler()
	: State(MakeShared<FState>())
{
}

FStrybokTickerScheduler::~FStrybokTickerScheduler()
{
	std::map<std::uint64_t, FTimer> Timers;
	Timers.swap(State->Timers);
	for (std::pair<const std::uint64_t, FTimer>& Entry : Timers)
	{
		FTSTicker::RemoveTicker(Entry.second.Handle);
	}
}

std::uint64_t FStrybokTickerScheduler::Schedule(std::int64_t DelayMs, std::function<void()> Task)
{
	const std::uint64_t Id = ++NextId;

	State->Timers[Id].Task = std::move(Task);

	const float DelaySeconds = DelayMs > 0 ? static_cast<float>(static_cast<double>(DelayMs) / 1000.0) : 0.0f;
	const TWeakPtr<FState> WeakState = State;
	// The core ticker runs on the game thread and never fires from inside AddTicker.
	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakState, Id](float /*DeltaSeconds*/) -> bool
		{
			const TSharedPtr<FState> Pinned = WeakState.Pin();
			if (Pinned.IsValid())
			{
				const auto Found = Pinned->Timers.find(Id);
				if (Found != Pinned->Timers.end())
				{
					// Take the task out before running it: it may schedule or cancel other timers.
					const std::function<void()> Fired = std::move(Found->second.Task);
					Pinned->Timers.erase(Found);
					if (Fired)
					{
						Fired();
					}
				}
			}
			return false; // one-shot
		}),
		DelaySeconds);

	const auto Stored = State->Timers.find(Id);
	if (Stored != State->Timers.end())
	{
		Stored->second.Handle = Handle;
	}
	return Id;
}

void FStrybokTickerScheduler::Cancel(std::uint64_t TimerId)
{
	const auto Found = State->Timers.find(TimerId);
	if (Found != State->Timers.end())
	{
		const FTSTicker::FDelegateHandle Handle = Found->second.Handle;
		State->Timers.erase(Found);
		FTSTicker::RemoveTicker(Handle);
	}
}

// ---------------------------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------------------------

std::int64_t FStrybokClock::NowMs()
{
	return static_cast<std::int64_t>(FPlatformTime::Seconds() * 1000.0);
}
