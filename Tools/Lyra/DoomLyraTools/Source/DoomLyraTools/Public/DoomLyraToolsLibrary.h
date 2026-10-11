// Editor scripting helper for DoomUE's Lyra level builder: Python honours EditDefaultsOnly and
// protected access, so properties such as ALyraWorldSettings::DefaultGameplayExperience (edited in
// the World Settings panel) cannot be set from a script without this.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DoomLyraToolsLibrary.generated.h"

UCLASS()
class DOOMLYRATOOLS_API UDoomLyraToolsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Sets a property of an object from its text form (as copy / paste in the details panel would). */
	UFUNCTION(BlueprintCallable, Category = "Doom Lyra Tools")
	static bool SetPropertyFromText(UObject* Object, FName PropertyName, const FString& Text);
};
