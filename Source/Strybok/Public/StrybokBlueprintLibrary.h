#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StrybokTypes.h"
#include "StrybokBlueprintLibrary.generated.h"

/** Blueprint helpers for reading a resolved link. */
UCLASS()
class STRYBOK_API UStrybokBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Reads one top-level field of the link payload as text, e.g. "invitationId".
	 * Returns false when the field is missing, null, an object or an array.
	 */
	UFUNCTION(BlueprintPure, Category = "Strybok", meta = (DisplayName = "Get Payload String"))
	static bool GetPayloadString(const FStrybokResolvedLink& Link, const FString& Key, FString& Value);

	/** The route split at "/": "/invite/abc123" gives ["invite", "abc123"]. */
	UFUNCTION(BlueprintPure, Category = "Strybok", meta = (DisplayName = "Get Route Segments"))
	static TArray<FString> GetRouteSegments(const FStrybokResolvedLink& Link);
};
