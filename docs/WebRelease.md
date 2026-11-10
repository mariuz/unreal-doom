# The web build and its release

Each published release gets a web build: DoomUE packaged for UE 5.9's **Wasm** platform
(WebAssembly with WebGPU, in Chrome, Edge and Safari), published on GitHub Pages at
https://mariuz.github.io/unreal-doom/ and attached to the release as `DoomUE-web.zip`.

## How a release goes

1. Tag and publish a release on GitHub (Releases > Draft a new release, or `gh release create vX.Y`).
2. The `Web release` workflow (`.github/workflows/web-release.yml`) starts on the self-hosted
   runner: it checks out the tag and runs `Tools\package_web.bat` (a Shipping build, cooked for the
   web, checked with the engine's `check_stage.py`).
3. It uploads the stage as the Pages artifact, attaches the zip to the release, and a second job
   on a GitHub-hosted runner publishes it to Pages.

A rebuild without a new release: Actions > Web release > Run workflow, with the tag.

## One-time setup (the owner)

These need the repository owner's GitHub login, so they are done by hand.

### The self-hosted runner

GitHub's hosted runners have no Unreal Engine, so the web build runs on a machine that has one.

1. **Settings > Actions > Runners > New self-hosted runner**, Windows x64. Follow its download
   and `config.cmd` steps in a folder such as `F:\actions-runner`.
2. When `config.cmd` asks for labels, add `unreal-5.9` (the workflow's `runs-on` needs
   `self-hosted`, `Windows`, `X64` and `unreal-5.9`).
3. Run it as a service (`config.cmd` offers it), or start `run.cmd` before a release.
4. The runner needs, as system or user environment variables (restart the runner after setting
   them):
   * `UE_ENGINE=F:\EpicGames\UE_5.9\Engine` (the tools default to `C:\Program Files\Epic Games\UE_5.9\Engine`);
   * `WASM_EMSDK_ROOT=F:\Tools\emsdk` (emscripten **6.0.10**, the only version 5.9 accepts:
     `emsdk install 6.0.10` and `emsdk activate 6.0.10`);
   * `gh` (GitHub CLI) on `PATH`, for attaching the zip.
5. **Keep it to releases.** The repository is public: a self-hosted runner must never run code
   from a fork's pull request. The workflow runs only on `release` and `workflow_dispatch`, which
   need write access. Also set **Settings > Actions > General > Fork pull request workflows** to
   require approval for all outside collaborators.
6. Close the DoomUE editor before a release: its Live Coding blocks the command-line build.

### GitHub Pages

**Settings > Pages > Build and deployment > Source: GitHub Actions.** The first run creates the
`github-pages` environment.

## Why these settings (`Config/DefaultEngine.ini`, `[/Script/WasmTargetPlatform.WasmTargetSettings]`)

* `bInjectIsolationHeaders=True`: the game runs on wasm threads, which need a cross-origin
  isolated page (COOP and COEP headers). GitHub Pages can't send headers, so the engine's service
  worker adds them; the page reloads once on the first visit.
* `StartMode=Click`: browsers allow sound and pointer lock only after a click.
* `bTargetMobileDeferredTier=False`: the web has the mobile renderer only (no Lumen, no Nanite),
  and the classic renderer is unlit, so the forward tier draws it the same on every GPU. The modern
  renderer is a Windows feature.
* `NativeBuildUrl`: a browser that can't run the page is sent to the releases.

## What the first build measured (UE 5.9.0, November 2026)

* Shipping stage: 273 MB in 50 files; `DoomUE-Wasm-Shipping.wasm` is 135 MB (25 MB as `.br`), the
  two Freedoom WADs 27 MB each, the cooked content 25 MB. The zip is 139 MB.
* In Edge (the harness's `boot` fixture): READY 14.8 s after the page opened, 104 MB downloaded
  before it, no WebGPU errors; the title screen. A Development stage with `-warp=E1M1` plays the
  level on the classic renderer.
* `check_stage.py` passes, release scan included (its one note: a font path inside Epic's own
  engine content).

## By hand, on this machine

```bat
set UE_ENGINE=F:\EpicGames\UE_5.9\Engine
set WASM_EMSDK_ROOT=F:\Tools\emsdk
Tools\package_web.bat Saved\Web Development
"%UE_ENGINE%\Binaries\ThirdParty\Python3\Win64\python.exe" "%UE_ENGINE%\Platforms\Wasm\Build\Tools\web\serve.py" Saved\Web\Wasm
```

Development accepts UE arguments in the URL (`?args=-warp=E1M1`); Shipping is the public build.
The engine's browser harness boots a stage and saves a screenshot (`final.png`); it looks for
Chrome, so on a machine with Edge only point it there:

```bat
set PROBE_CHROME=C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe
"%UE_ENGINE%\Binaries\ThirdParty\Python3\Win64\python.exe" -I "%UE_ENGINE%\Platforms\Wasm\Build\Tools\web\webrun.py" run --stage Saved\Web\Wasm --fixture boot --out Saved\Web\run_boot
```

The engine's guides are in `Engine/Platforms/Wasm/Docs` (GettingStarted, Deploy, Troubleshooting).
