@echo off
rem Tries the project on an engine version: builds both targets, regenerates the generated assets
rem (instead of resaving the old ones), rebakes the tiles, runs data validation, the WAD checks and
rem the automation tests. Stops at the first failure. See docs/EngineUpgrade.md.
rem
rem   set UE_ENGINE=C:\Program Files\Epic Games\UE_5.9\Engine
rem   Tools\upgrade_check.bat                 every step, every test
rem   Tools\upgrade_check.bat DoomUE.Map      every step, one test group
rem   Tools\upgrade_check.bat DoomUE -NullRHI no GPU: the DoomUE.Render tests skip
rem
rem UE_ENGINE overrides the engine folder (default: C:\Program Files\Epic Games\UE_5.8\Engine).
rem Regenerating rewrites tracked assets in Content/Doom: review them with git diff afterwards.
setlocal
set ENGINE=C:\Program Files\Epic Games\UE_5.8\Engine
if not "%UE_ENGINE%"=="" set ENGINE=%UE_ENGINE%
set UE_ENGINE=%ENGINE%
set PROJECT=%~dp0..\DoomUE.uproject
set EDITOR="%ENGINE%\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJECT%" -unattended -nosplash -nopause
set FILTER=%~1
if "%FILTER%"=="" set FILTER=DoomUE
echo Engine: %ENGINE%
findstr "MajorVersion MinorVersion PatchVersion" "%ENGINE%\Build\Build.version"

set STEP=build the editor
echo. & echo === %STEP%
call "%ENGINE%\Build\BatchFiles\Build.bat" DoomUEEditor Win64 Development -Project="%PROJECT%" -WaitMutex -NoHotReload || goto failed

set STEP=build the game
echo. & echo === %STEP%
call "%ENGINE%\Build\BatchFiles\Build.bat" DoomUE Win64 Development -Project="%PROJECT%" -WaitMutex -NoHotReload || goto failed

set STEP=materials and effects
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -force || goto failed

set STEP=AI assets
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -ai -force || goto failed

set STEP=input assets
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -input -force || goto failed

set STEP=imported levels' tiles
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -rebaketiles=all || goto failed

set STEP=WAD tile sets
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -baketiles=all || goto failed

set STEP=data validation
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -validate || goto failed

set STEP=node builder against every map
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -nodetest=all || goto failed

set STEP=PWAD export round trip
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -wadtest=all || goto failed

set STEP=random map generator
echo. & echo === %STEP%
%EDITOR% -run=DoomContent -gentest=20 || goto failed

set STEP=automation tests %FILTER%
echo. & echo === %STEP%
call "%~dp0run_tests.bat" %FILTER% %2 %3 || goto failed

echo.
echo Upgrade check passed on %ENGINE%. Next: git diff Content (expected: regenerated assets only),
echo -perftest against the README tables, a packaged build and a hand play (docs/EngineUpgrade.md).
exit /b 0

:failed
echo.
echo === FAILED: %STEP%
exit /b 1
