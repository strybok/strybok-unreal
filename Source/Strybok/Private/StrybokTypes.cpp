#include "StrybokTypes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

TSharedPtr<FJsonObject> FStrybokResolvedLink::GetPayloadObject() const
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(PayloadJson);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		Object = MakeShared<FJsonObject>();
	}
	return Object;
}

bool FStrybokResolvedLink::GetPayloadString(const FString& Key, FString& OutValue) const
{
	const TSharedPtr<FJsonObject> Object = GetPayloadObject();
	FString Value;
	if (Object.IsValid() && Object->TryGetStringField(Key, Value))
	{
		OutValue = MoveTemp(Value);
		return true;
	}
	return false;
}

TArray<FString> FStrybokResolvedLink::GetRouteSegments() const
{
	TArray<FString> Segments;
	RoutePath.ParseIntoArray(Segments, TEXT("/"), true);
	return Segments;
}
