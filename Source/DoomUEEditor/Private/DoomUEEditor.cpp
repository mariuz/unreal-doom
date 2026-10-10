#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "DoomContentBuilder.h"

class FDoomUEEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// First editor launch: generate the materials the game needs once the engine is up.
		if (GIsEditor && !IsRunningCommandlet())
		{
			FCoreDelegates::OnPostEngineInit.AddLambda([]()
			{
				if (!DoomContent::AssetsExist())
				{
					DoomContent::Build(false);
				}
			});
		}
	}
};

IMPLEMENT_MODULE(FDoomUEEditorModule, DoomUEEditor);
