#include "DoomGameState.h"
#include "Net/UnrealNetwork.h"

void ADoomGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADoomGameState, Flow);
}
