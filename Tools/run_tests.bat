@echo off
rem Runs the DoomUE automation tests (Source/DoomUEEditor/Private/Tests) and exits non-zero on failure.
rem
rem   Tools\run_tests.bat                     every test, with rendering (screenshot comparisons too)
rem   Tools\run_tests.bat DoomUE -NullRHI     no GPU needed (CI agents); the DoomUE.Render tests skip
rem   Tools\run_tests.bat DoomUE.Game         one group: DoomUE.Map, .Patches, .Game, .Editor, .Render
rem
rem UE_ENGINE overrides the engine folder (default: C:\Program Files\Epic Games\UE_5.8\Engine).
rem The report goes to Saved\Automation\Report (index.html, index.json).
setlocal
set ENGINE=C:\Program Files\Epic Games\UE_5.8\Engine
if not "%UE_ENGINE%"=="" set ENGINE=%UE_ENGINE%
set FILTER=%~1
if "%FILTER%"=="" set FILTER=DoomUE
set PROJECT=%~dp0..\DoomUE.uproject
set REPORT=%~dp0..\Saved\Automation\Report
"%ENGINE%\Binaries\Win64\UnrealEditor.exe" "%PROJECT%" -ExecCmds="Automation RunTests %FILTER%; Quit" -unattended -nopause -nosplash -windowed -resx=1280 -resy=720 -testexit="Automation Test Queue Empty" -ReportExportPath="%REPORT%" -log %2 %3 %4
set RESULT=%ERRORLEVEL%
if %RESULT%==0 (echo DoomUE tests passed. Report: %REPORT%\index.html) else (echo DoomUE tests FAILED ^(exit %RESULT%^). Report: %REPORT%\index.html)
exit /b %RESULT%
