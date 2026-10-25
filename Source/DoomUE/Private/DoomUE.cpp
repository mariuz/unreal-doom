#include "DoomUE.h"
#include "Modules/ModuleManager.h"
#include "GameFramework/Actor.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"

DEFINE_LOG_CATEGORY(LogDoom);
DEFINE_LOG_CATEGORY(LogDoomGame);
DEFINE_LOG_CATEGORY(LogDoomRender);
DEFINE_LOG_CATEGORY(LogDoomAudio);
DEFINE_LOG_CATEGORY(LogDoomNet);
DEFINE_LOG_CATEGORY(LogDoomAI);
DEFINE_LOG_CATEGORY(LogDoomEditor);
DEFINE_LOG_CATEGORY(LogDoomTest);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, DoomUE, "DoomUE");

float Doom::HalfHeight(const AActor* Actor)
{
	if (!Actor)
	{
		return 0.f;
	}
	if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Actor->GetRootComponent()))
	{
		return Capsule->GetScaledCapsuleHalfHeight();
	}
	if (const USphereComponent* Sphere = Cast<USphereComponent>(Actor->GetRootComponent()))
	{
		return Sphere->GetScaledSphereRadius();
	}
	return Actor->GetSimpleCollisionHalfHeight();
}
