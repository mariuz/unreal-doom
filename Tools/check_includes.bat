@echo off
rem The include and unity-name checks, Epic's IWYU discipline:
rem   1. Tools\check_unity_names.py: names a .cpp keeps to itself (anonymous namespaces, file-level
rem      static) that another .cpp of the module also uses; in a unity build they collide.
rem   2. A build with no unity files and no precompiled headers: every .cpp must compile on its own,
rem      with the includes it uses (a unity or shared-PCH build hides a missing one).
rem
rem   Tools\check_includes.bat            both
rem   Tools\check_includes.bat names      only the name check (seconds)
rem
rem UE_ENGINE overrides the engine folder (default: C:\Program Files\Epic Games\UE_5.8\Engine).
rem The strict build writes its own intermediate files and takes a while (about 10 minutes the first
rem time); close the editor first (Live Coding blocks command-line builds).
setlocal
set ENGINE=C:\Program Files\Epic Games\UE_5.8\Engine
if not "%UE_ENGINE%"=="" set ENGINE=%UE_ENGINE%
set PROJECT=%~dp0..\DoomUE.uproject

python "%~dp0check_unity_names.py"
set NAMES=%ERRORLEVEL%
if /I "%~1"=="names" exit /b %NAMES%

call "%ENGINE%\Build\BatchFiles\Build.bat" DoomUEEditor Win64 Development -Project="%PROJECT%" -WaitMutex -NoHotReload -DisableUnity -NoPCH -NoSharedPCH
set BUILD=%ERRORLEVEL%
if %NAMES%==0 if %BUILD%==0 (echo Include and unity-name checks passed.& exit /b 0)
echo Include and unity-name checks FAILED (names %NAMES%, strict build %BUILD%).
exit /b 1
