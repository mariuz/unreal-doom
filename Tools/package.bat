@echo off
rem Packages DoomUE for Win64, with baked static-mesh tiles for every map of the IWAD, so the modern
rem renderer has distance fields in maps played straight from the WAD too.
rem
rem   Tools\package.bat [archive folder] [extra -run=DoomContent options, e.g. -iwad=freedoom2.wad]
rem
rem UE_ENGINE overrides the engine folder (default: C:\Program Files\Epic Games\UE_5.8\Engine).
setlocal
set ENGINE=C:\Program Files\Epic Games\UE_5.8\Engine
if not "%UE_ENGINE%"=="" set ENGINE=%UE_ENGINE%
set PROJECT=%~dp0..\DoomUE.uproject
set ARCHIVE=%~1
if "%ARCHIVE%"=="" set ARCHIVE=%~dp0..\..\DoomUE_Package

echo Baking tile sets (Content/Doom/Tiles)...
"%ENGINE%\Binaries\Win64\UnrealEditor-Cmd.exe" "%PROJECT%" -run=DoomContent -baketiles=all -unattended -nosplash %2 %3
if errorlevel 1 (echo Baking tiles FAILED & exit /b 1)

echo Packaging to %ARCHIVE%...
call "%ENGINE%\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="%PROJECT%" -noP4 -platform=Win64 -clientconfig=Development -build -cook -stage -pak -archive -archivedirectory="%ARCHIVE%"
exit /b %ERRORLEVEL%
