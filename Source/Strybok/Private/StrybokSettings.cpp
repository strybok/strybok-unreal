#include "StrybokSettings.h"

UStrybokSettings::UStrybokSettings()
{
}

FName UStrybokSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

FStrybokConfig UStrybokSettings::ToConfig() const
{
	FStrybokConfig Config;
	Config.ApiBaseUrl = ApiBaseUrl;
	if (!LinkHost.TrimStartAndEnd().IsEmpty())
	{
		Config.AllowedHosts.Add(LinkHost);
	}
	Config.AllowedHosts.Append(AdditionalAllowedHosts);
	Config.TimeoutSeconds = TimeoutSeconds;
	Config.MaxRetries = MaxRetries;
	Config.RetryDelaySeconds = RetryDelaySeconds;
	Config.DedupeWindowSeconds = DedupeWindowSeconds;
	Config.bReportAppOpens = bReportAppOpens;
	Config.ClientKey = ClientKey;
	Config.bDeferredLinks = bDeferredLinks;
	Config.bAllowProbabilisticMatching = bAllowProbabilisticMatching;
	Config.AndroidPackageName = AndroidPackageName;
	Config.IosBundleId = IosBundleId;
	return Config;
}
