#include "DoomLyraToolsLibrary.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, DoomLyraTools);

bool UDoomLyraToolsLibrary::SetPropertyFromText(UObject* Object, FName PropertyName, const FString& Text)
{
	FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(PropertyName) : nullptr;
	if (!Property)
	{
		return false;
	}
	Object->Modify();
	Object->PreEditChange(Property);
	const bool bOk = Property->ImportText_InContainer(*Text, Object, Object, PPF_None) != nullptr;
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(Event);
	return bOk;
}

TArray<FString> UDoomLyraToolsLibrary::ListProperties(UObject* Object, bool bBlueprintOnly)
{
	TArray<FString> Out;
	if (!Object)
	{
		return Out;
	}
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		const UClass* Owner = It->GetOwnerClass();
		if (bBlueprintOnly && (!Owner || Owner->HasAnyClassFlags(CLASS_Native)))
		{
			continue;
		}
		FString Value;
		It->ExportTextItem_InContainer(Value, Object, nullptr, Object, PPF_None);
		Out.Add(FString::Printf(TEXT("%s: %s = %s"), *It->GetName(), *It->GetCPPType(), *Value.Left(200)));
	}
	return Out;
}
