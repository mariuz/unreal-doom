// The scripted play-test (-playtest, DoomPlayTest.cpp) as automation tests: one test per step,
// DoomUE.PlayTest.<step>, each on Freedoom 1's E1M1 in PIE. A step that skips (the map lacks what
// it needs) passes with a warning; the step's own details go into the test's log.
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DoomGameMode.h"
#include "DoomTestCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace DoomTest;

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDoomPlayTestStep, "DoomUE.PlayTest", DoomTest::Flags)

void FDoomPlayTestStep::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const FString& Step : ADoomGameMode::PlayTestStepNames())
	{
		OutBeautifiedNames.Add(Step);
		OutTestCommands.Add(Step);
	}
}

bool FDoomPlayTestStep::RunTest(const FString& Step)
{
	StartGame(this);
	LoadMap(this, TEXT("E1M1"));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("settle"), Wait(0.5f)));
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("start ") + Step, [Step]()
	{
		PlayWorld()->GetAuthGameMode<ADoomGameMode>()->StartPlayTest(Step, false);
		return true;
	}));
	// altdeath waits 30 s of game time for an item to come back; the rest take a few seconds
	ADD_LATENT_AUTOMATION_COMMAND(FStep(this, TEXT("play ") + Step, [this]()
	{
		const ADoomGameMode* GM = PlayWorld()->GetAuthGameMode<ADoomGameMode>();
		if (!GM->IsPlayTestDone())
		{
			return false;
		}
		TestEqual(TEXT("one step ran"), GM->GetPlayTestResults().Num(), 1);
		for (const FDoomPlayTestResult& R : GM->GetPlayTestResults())
		{
			const FString Line = FString::Printf(TEXT("%s %s: %s"), *R.Step, *R.Result, *R.Details);
			if (R.Result == TEXT("FAIL"))
			{
				AddError(Line);
			}
			else if (R.Result == TEXT("SKIP"))
			{
				AddWarning(Line);
			}
			else
			{
				AddInfo(Line);
			}
		}
		return true;
	}, 120.f));
	EndGame(this);
	return true;
}

#endif
