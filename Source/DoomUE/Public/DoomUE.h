// DoomUE: DOOM 1 / DOOM 2 on Unreal Engine 5.8.
#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogDoom, Log, All);

/**
 * Coordinates.
 * Doom maps are right-handed (X east, Y north, Z up); Unreal is left-handed
 * (X forward, Y right, Z up). We flip Y and scale: one Doom map unit is
 * DoomScale Unreal units (cm), so the 56-unit marine is 1.68 m tall and
 * Unreal's physics, Lumen and audio attenuation work at sensible sizes.
 */
namespace Doom
{
	constexpr float Scale = 3.0f;
	constexpr int32 TicRate = 35;
	constexpr float TicSeconds = 1.0f / TicRate;

	inline FVector ToUE(float X, float Y, float Z) { return FVector(X * Scale, -Y * Scale, Z * Scale); }
	inline FVector ToUE(const FVector2f& P, float Z) { return ToUE(P.X, P.Y, Z); }
	inline FVector2f ToDoom2D(const FVector& V) { return FVector2f(V.X / Scale, -V.Y / Scale); }
	inline float ToDoomZ(float Z) { return Z / Scale; }
	/** Doom angle (degrees CCW from east) to Unreal yaw (degrees CW from +X). */
	inline float AngleToYaw(float DoomDegrees) { return -DoomDegrees; }
	inline float YawToAngle(float Yaw) { return -Yaw; }
}
