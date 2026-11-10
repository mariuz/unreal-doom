@echo off
rem Packages DoomUE for the web (UE 5.9's Wasm platform: WebAssembly with WebGPU) as a static site.
rem The release workflow (.github/workflows/web-release.yml) runs it on the self-hosted runner, then
rem publishes the stage to GitHub Pages. See docs/WebRelease.md.
rem
rem   Tools\package_web.bat [out folder] [Shipping|Development]
rem
rem The stage lands in <out>\Wasm (default: Saved\Web\Wasm), and <out>\DoomUE-web.zip holds it.
rem Development accepts ?args= in the page's URL, for iterating; Shipping is the public build.
rem
rem UE_ENGINE overrides the engine folder (default: C:\Program Files\Epic Games\UE_5.9\Engine).
rem WASM_EMSDK_ROOT must point at an activated emsdk 6.0.10 (the only version 5.9 accepts).
setlocal
set ENGINE=C:\Program Files\Epic Games\UE_5.9\Engine
if not "%UE_ENGINE%"=="" set ENGINE=%UE_ENGINE%
set PROJECT=%~dp0..\DoomUE.uproject
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0..\Saved\Web
rem absolute: UAT runs from the engine's folder
for %%I in ("%OUT%") do set OUT=%%~fI
set CONFIG=%~2
if "%CONFIG%"=="" set CONFIG=Shipping
set PYTHON="%ENGINE%\Binaries\ThirdParty\Python3\Win64\python.exe"
set WASMTOOLS=%ENGINE%\Platforms\Wasm\Build\Tools

if "%WASM_EMSDK_ROOT%"=="" if "%EMSDK%"=="" (
	echo WASM_EMSDK_ROOT is not set: install emsdk 6.0.10 ^(emsdk install 6.0.10, emsdk activate 6.0.10^)
	exit /b 1
)
if not exist "%ENGINE%\Platforms\Wasm" (
	echo %ENGINE% has no Wasm platform: it needs UE 5.9 or later
	exit /b 1
)

echo Packaging DoomUE for the web (%CONFIG%) into %OUT%...
if exist "%OUT%\Wasm" rmdir /s /q "%OUT%\Wasm"
call "%ENGINE%\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="%PROJECT%" -noP4 -platform=Wasm -clientconfig=%CONFIG% ^
	-build -cook -stage -pak -iostore -package -archive -archivedirectory="%OUT%" -unattended -utf8output ^
	-AdditionalCookerOptions="-unattended -nopause -nosplash -nullrhi"
if errorlevel 1 (echo Packaging FAILED & exit /b 1)

echo Checking the stage...
%PYTHON% -I "%WASMTOOLS%\deploy\check_stage.py" "%OUT%\Wasm"
if errorlevel 1 (echo The stage check FAILED & exit /b 1)

echo Zipping the stage for the release...
if exist "%OUT%\DoomUE-web.zip" del "%OUT%\DoomUE-web.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\Wasm\*' -DestinationPath '%OUT%\DoomUE-web.zip'"
if errorlevel 1 (echo Zipping FAILED & exit /b 1)

echo Web build ready: %OUT%\Wasm (serve it with %WASMTOOLS%\web\serve.py), %OUT%\DoomUE-web.zip
exit /b 0
