#include "StrybokPlatformLinks.h"

#include <atomic>
#include <mutex>
#include <vector>

#if PLATFORM_IOS
// Defined in IOS/StrybokIOSLinks.cpp.
void StrybokIOSInstallUniversalLinkHook();
#endif

namespace
{
	/** More than enough: URLs only pile up while the SDK is not configured. */
	const size_t GStrybokMaxBufferedUrls = 16;

	// Function-local statics: Push can run before this file's static initialisers (Objective-C
	// +load) and the objects are deliberately never destroyed (late native callbacks at exit).
	std::mutex& StrybokUrlMutex()
	{
		static std::mutex* Mutex = new std::mutex();
		return *Mutex;
	}

	std::vector<std::string>& StrybokUrlBuffer()
	{
		static std::vector<std::string>* Buffer = new std::vector<std::string>();
		return *Buffer;
	}

	std::atomic<bool> GStrybokLaunchClaimed(false);
}

void FStrybokPlatformLinks::Push(const std::string& Utf8Url)
{
	if (Utf8Url.empty())
	{
		return;
	}
	std::lock_guard<std::mutex> Lock(StrybokUrlMutex());
	std::vector<std::string>& Buffer = StrybokUrlBuffer();
	if (Buffer.size() >= GStrybokMaxBufferedUrls)
	{
		Buffer.erase(Buffer.begin());
	}
	Buffer.push_back(Utf8Url);
}

TArray<FString> FStrybokPlatformLinks::Drain()
{
	std::vector<std::string> Taken;
	{
		std::lock_guard<std::mutex> Lock(StrybokUrlMutex());
		Taken.swap(StrybokUrlBuffer());
	}
	TArray<FString> Result;
	Result.Reserve(static_cast<int32>(Taken.size()));
	for (const std::string& Url : Taken)
	{
		Result.Add(FString(UTF8_TO_TCHAR(Url.c_str())));
	}
	return Result;
}

bool FStrybokPlatformLinks::ClaimLaunch()
{
	return !GStrybokLaunchClaimed.exchange(true);
}

void FStrybokPlatformLinks::InstallHooks()
{
#if PLATFORM_IOS
	StrybokIOSInstallUniversalLinkHook();
#endif
	// Android needs no installation: GameActivity calls the JNI function in
	// Android/StrybokAndroidLinks.cpp (Java glue added by Strybok_UPL.xml).
}
