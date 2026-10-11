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
