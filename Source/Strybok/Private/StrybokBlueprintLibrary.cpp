#include "StrybokBlueprintLibrary.h"

bool UStrybokBlueprintLibrary::GetPayloadString(const FStrybokResolvedLink& Link, const FString& Key, FString& Value)
{
	Value.Reset();
	return Link.GetPayloadString(Key, Value);
}

TArray<FString> UStrybokBlueprintLibrary::GetRouteSegments(const FStrybokResolvedLink& Link)
{
	return Link.GetRouteSegments();
}

bool UStrybokBlueprintLibrary::HasCampaign(const FStrybokResolvedLink& Link)
{
	return !Link.Campaign.IsEmpty();
}
