@echo off
rem Static analysis: a build with MSVC's code analysis (/analyze) over both modules. Fails when it
rem reports any warning in the project's own files (the engine's headers are not analysed).
rem
rem   Tools\analyze.bat
rem
rem UE_ENGINE overrides the engine folder (default: C:\Program Files\Epic Games\UE_5.8\Engine).
rem It recompiles everything the first time (about 17 minutes); close the editor first (Live Coding
rem blocks command-line builds). The output goes to Saved\analyze.log.
setlocal
set ENGINE=C:\Program Files\Epic Games\UE_5.8\Engine
if not "%UE_ENGINE%"=="" set ENGINE=%UE_ENGINE%
set ROOT=%~dp0..
set LOG=%ROOT%\Saved\analyze.log
if not exist "%ROOT%\Saved" mkdir "%ROOT%\Saved"

call "%ENGINE%\Build\BatchFiles\Build.bat" DoomUEEditor Win64 Development -Project="%ROOT%\DoomUE.uproject" -WaitMutex -NoHotReload -StaticAnalyzer=VisualCpp > "%LOG%" 2>&1
set BUILD=%ERRORLEVEL%
findstr /R /C:": warning C[0-9]" "%LOG%"
if %ERRORLEVEL%==0 (echo Static analysis FAILED: warnings above, see Saved\analyze.log.& exit /b 1)
if not %BUILD%==0 (echo Static analysis FAILED: the build failed, see Saved\analyze.log.& exit /b 1)
echo Static analysis passed: no warnings.
exit /b 0
