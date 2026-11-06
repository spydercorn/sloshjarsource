# Filled Jar v13.1 — movement-triggered slosh (b14 compilation fix)

## What v13.1 fixes

The original v13 `FilledJarLiquid.hlsl` declared a constant buffer at `register(b14)`. Shader Model 5.0/D3D11 only permits constant-buffer slots 0–13, so **every shader entry point failed to compile** (`X4567: maximum cbuffer exceeded`) and the liquid overlay never ran.

- The HLSL movement buffer now uses **PS b12**, with world data still at **PS b13**.
- The C++ second-pass overlay now captures, binds, and restores **PS b12**, and adds a compile-time slot-range assertion.
- Both new source files must be used together for active movement-based slosh: **rebuild the DLL** using `Build-Windows.ps1`, then install the DLL, INI, and HLSL.
- This package contains **source code only**. It does not contain a newly compiled Windows DLL. Shader compilation and in-game behavior could not be tested in this environment.

### Restore stationary liquid immediately (no DLL rebuild)

If you are still using the original **v13 DLL**, copy `static-liquid-hotfix/SKSE/Plugins/FilledJar/FilledJarLiquid.hlsl` to `Data/SKSE/Plugins/FilledJar/FilledJarLiquid.hlsl` (or the equivalent path in your mod manager), replacing the installed HLSL. Leave the existing DLL and INI as they are. Restart Skyrim.

This variant uses valid `b12` syntax but sets `FJ_SLOSH_ENABLED=0`, so it should restore the v11-style **static, volume-conserving liquid** without relying on the old DLL's invalid PS b14 binding. **It will not slosh with movement.** For movement slosh, use the v13.1 rebuilt DLL with the normal HLSL in `SKSE/Plugins/FilledJar/` instead.

Do not install both HLSL variants into the same mod folder; one is a temporary rescue option, the other needs the rebuilt DLL.

### Expected log after the full v13.1 rebuild

Look for `LIQUID SHADER PROBE: ... COMPILED` and `gravity=OK, ... gravityGPU=READY`, followed by `LIQUID OVERLAY: DRAW #...` once a tagged jar is on screen. Movement kicks appear only when motion is strong enough. Any remaining compilation errors or overlay skips should be investigated from the new log.

## What it does

- Reuses the successful v11 volume-conserving liquid + v9 measured Jar03 shape and second-pass overlay.
- Samples **each marked jar's own scene-graph world transform** during the existing known render pass.
- Changes in horizontal velocity and jar-axis rotation kick a damped spring that moves a virtual liquid plane.
- The spring makes liquid rock **after movement stops** and gradually settle. Different jars move independently.
- Keeps the surface cap, wall coloring, and approximate liquid volume on the **same plane**.
- Preserves Skyrim/Open Shaders' original glass and restores the previous D3D11 PS b12 binding after the overlay.

This version requires a DLL rebuild; an HLSL-only patch cannot persist per-jar motion history. It is built for the user's observed Skyrim SE/AE Jar03 528-index pass, **not a general-purpose Skyrim VR renderer**.

## Install / build

1. BACK UP your stable v11 DLL, HLSL and INI. Do not save during this first experiment.
2. Extract this ZIP over your existing `FilledJar_Source` folder **but keep `work/`** from your previous builds.
3. In **Developer PowerShell for VS 2022**, run each separately:
   ```powershell
   $env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
   .\Build-Windows.ps1
   ```
4. Install ALL THREE new files from `install/SKSE/Plugins`: `FilledJarMarkerDetector.dll`, `FilledJarVisualTest.ini`, `FilledJar/FilledJarLiquid.hlsl`. Do not pair an older DLL with this movement-enabled HLSL; the fixed source builds a DLL that writes PS b12.
5. In-game pick up a tagged jar, **move sideways, stop suddenly, and rotate it**, then set it down. It should keep rocking a little and settle. A stationary jar should not loop forever.

## Tuning in FilledJarVisualTest.ini

- `Frequency=1.05`: natural angular frequency (rad/s), ~6 seconds per oscillation. `0.8` is slower (~7.9 seconds).
- `Damping=0.24`: damping ratio. `0.12` lasts longer; `0.45` settles sooner.
- `TranslationImpulse=0.0013`: response to horizontal acceleration in Skyrim units/s.
- `RotationImpulse=0.90`: response to changes in the jar's local +Z axis.
- `MaxTiltDegrees=16.0`: absolute visual safety limit (start low).
- `Enabled=0`: keeps the second-pass liquid but stops generating motion (zero slope).

Keep `[LiquidOverlay] Enabled=1`. In the HLSL, `#define FJ_SLOSH_ENABLED 0` also disables motion; changing HLSL requires restarting Skyrim. Changes to INI likewise require a restart.

## Diagnostics

`Documents/My Games/Skyrim Special Edition/SKSE/FilledJarMarkerDetector.log` should say `MOVEMENT SLOSH: enabled=true` and (when disturbed) `MOVEMENT SLOSH: kick #...`. The log caps kicks to 12 lines.

## Important limitations

- C++ source generated for existing CommonLibSSE-NG/Visual Studio setup. **Not compiled or in-game tested here**. Build errors or runtime issues must be checked on the user's machine.
- This is visual inertia, not a physical liquid solver. The current volume integration still runs in HLSL per pixel and can be expensive.
- Moving the camera alone shouldn't excite jars because motion is sampled from geometry world transforms.
- The plugin observes geometry only while rendered; if a jar goes unseen for more than 0.4 seconds, it won't infer the motion missed during that interval.
- Renderer vtable hooks, other shader mods and Skyrim VR compatibility remain experimental.

## Revert

Restore your complete v11 DLL + HLSL + INI backup, or set `[MovementSlosh] Enabled=0` to stop sloshing without replacing the DLL.
