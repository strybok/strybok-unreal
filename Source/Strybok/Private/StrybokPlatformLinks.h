#pragma once

#include "CoreMinimal.h"

#include <string>

/**
 * Hand-over point between native code (iOS app delegate, Android activity) and the game thread.
 *
 * Native callbacks can fire before the engine is initialised and on other threads, so the buffer
 * uses the C++ standard library only. The subsystem drains it once per frame on the game thread.
 */
class FStrybokPlatformLinks
{
public:
	/** Any thread, any time (also before engine start-up). Keeps the most recent URLs only. */
	static void Push(const std::string& Utf8Url);

	/** Game thread: takes every URL received since the last call, oldest first. */
	static TArray<FString> Drain();

	/**
	 * True exactly once per process: the first configured subsystem treats the oldest buffered URL
	 * as the launch link.
	 */
	static bool ClaimLaunch();

	/** Installs the native hooks of the current platform. Idempotent. */
	static void InstallHooks();
};
