#include "Modules/ModuleManager.h"
#include "StrybokLog.h"
#include "StrybokPlatformLinks.h"

DEFINE_LOG_CATEGORY(LogStrybok);

class FStrybokModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// On iOS the hook is normally installed long before this, from an Objective-C +load method
		// (see IOS/StrybokIOSLinks.cpp). Calling it again is harmless and keeps that file linked.
		FStrybokPlatformLinks::InstallHooks();
	}
};

IMPLEMENT_MODULE(FStrybokModule, Strybok)
