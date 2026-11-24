#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"
#include "StrybokCore/StrybokCore.h"
#include "StrybokTypes.h"

#include <map>
#include <string>

/** UTF-8 <-> FString, the only string conversions between the core and the engine. */
namespace StrybokConvert
{
	std::string ToStd(const FString& Value);
	FString ToFString(const std::string& Value);
	FStrybokResolvedLink ToLink(const strybok::ResolvedLink& Link);
	FStrybokError ToError(const strybok::LinkError& Error);
	strybok::Config ToCoreConfig(const FStrybokConfig& Config);
}

/** The core's HTTP transport on top of the engine's HTTP module. Game thread only. */
class FStrybokHttpTransport final : public strybok::IHttpTransport
{
public:
	FStrybokHttpTransport();
	virtual ~FStrybokHttpTransport() override;

	virtual std::uint64_t Send(const strybok::HttpRequest& Request, Callback OnDone) override;
	virtual void Cancel(std::uint64_t RequestId) override;

private:
	struct FPending
	{
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
		Callback OnDone;
	};

	// std::map on purpose: Unreal containers relocate elements with memcpy, which is not safe
	// for std::function (it may point into itself).
	struct FState
	{
		std::map<std::uint64_t, FPending> Pending;
	};

	static void Complete(const TWeakPtr<FState>& WeakState, std::uint64_t Id, const strybok::HttpResponse& Response);

	/** Shared with the completion lambdas, which may outlive this object. */
	TSharedRef<FState> State;
	std::uint64_t NextId = 0;
};

/** The core's timers (attempt timeout, retry back-off) on the engine's core ticker. Game thread only. */
class FStrybokTickerScheduler final : public strybok::IScheduler
{
public:
	FStrybokTickerScheduler();
	virtual ~FStrybokTickerScheduler() override;

	virtual std::uint64_t Schedule(std::int64_t DelayMs, std::function<void()> Task) override;
	virtual void Cancel(std::uint64_t TimerId) override;

private:
	struct FTimer
	{
		FTSTicker::FDelegateHandle Handle;
		std::function<void()> Task;
	};

	struct FState
	{
		std::map<std::uint64_t, FTimer> Timers;
	};

	TSharedRef<FState> State;
	std::uint64_t NextId = 0;
};

class FStrybokClock final : public strybok::IClock
{
public:
	virtual std::int64_t NowMs() override;
};

/**
 * Everything one configuration of the SDK owns. The engine is declared last so it is destroyed
 * first, while the transport and the scheduler it cancels its work on are still alive.
 */
struct FStrybokRuntime
{
	FStrybokHttpTransport Transport;
	FStrybokTickerScheduler Scheduler;
	FStrybokClock Clock;
	TUniquePtr<strybok::LinkEngine> Engine;
};
