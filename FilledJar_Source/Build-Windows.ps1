# Source-only helper for a Windows machine with Visual Studio, Git, CMake and vcpkg.
# It fetches the upstream CommonLibSSE-NG template, inserts our Plugin.cpp, and builds a DLL.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $root 'work\FilledJarMarkerDetector'
# Use a separate CMake cache: changing vcpkg triplet in an existing cache is unsafe.
# This keeps the original 'build' folder and downloaded sources intact.
$build = Join-Path $project 'build-static-md'

foreach ($tool in @('git', 'cmake')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing '$tool'. Please install Git and CMake first."
    }
}
if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
    throw 'Set VCPKG_ROOT to your vcpkg folder (with scripts\buildsystems\vcpkg.cmake).'
}
if (-not (Test-Path $project)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $project) | Out-Null
    & git clone --recurse-submodules https://github.com/codepuncher/CommonLibSSE-NG-template.git $project
    if ($LASTEXITCODE -ne 0) { throw 'Could not clone the CommonLibSSE-NG template' }
} elseif (-not (Test-Path (Join-Path $project 'lib\commonlibsse-ng\CMakeLists.txt'))) {
    Push-Location $project
    try {
        & git submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw 'Could not initialize CommonLibSSE-NG' }
    } finally { Pop-Location }
}
Copy-Item -LiteralPath (Join-Path $root 'source\Plugin.cpp') -Destination (Join-Path $project 'src\Plugin.cpp') -Force

# Template currently calls its target 'ExampleMod'. Rename it for a distinct plugin DLL/log.
$cmakeFile = Join-Path $project 'CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakeFile -Raw
$cmakeText = $cmakeText.Replace('ExampleMod', 'FilledJarMarkerDetector')
[System.IO.File]::WriteAllText($cmakeFile, $cmakeText)
$vcpkgFile = Join-Path $project 'vcpkg.json'
$vcpkgText = Get-Content -LiteralPath $vcpkgFile -Raw
$vcpkgText = $vcpkgText.Replace('examplemod', 'filledjarmarkerdetector')
[System.IO.File]::WriteAllText($vcpkgFile, $vcpkgText)

Write-Host 'Configuring static-library build (x64-windows-static-md): spdlog/fmt DLLs will not be needed at runtime.'
Write-Host 'This uses build-static-md; existing work/build and downloaded source remain untouched.'
& cmake -S $project -B $build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$($env:VCPKG_ROOT)\scripts\buildsystems\vcpkg.cmake" "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed; see the error above' }

# MSVC VS2022 14.36 + PCH: automatic C++23 STL module builds can produce
# std.ixx C1010. Apply the opt-out to BOTH the CommonLibSSE dependency AND
# this plugin target. Previous versions only patched CommonLibSSE.
# These generated .vcxproj files are safe to patch AFTER cmake configure;
# running this script again reapplies the setting.
$projectNames = @('CommonLibSSE.vcxproj', 'FilledJarMarkerDetector.vcxproj')
foreach ($projectName in $projectNames) {
    $found = Get-ChildItem -Path $build -Filter $projectName -File -Recurse | Select-Object -First 1
    if (-not $found) {
        throw "Could not find generated $projectName to apply C1010 fix"
    }

    [xml]$vcx = Get-Content -LiteralPath $found.FullName -Raw
    $optionsNodes = $vcx.SelectNodes("//*[local-name()='ItemDefinitionGroup']/*[local-name()='ClCompile']")
    if (-not $optionsNodes -or $optionsNodes.Count -lt 1) {
        throw "Could not locate C++ compiler settings in $projectName"
    }

    foreach ($options in $optionsNodes) {
        $setting = $options.SelectSingleNode("*[local-name()='BuildStlModules']")
        if (-not $setting) {
            $setting = $vcx.CreateElement('BuildStlModules', $vcx.DocumentElement.NamespaceURI)
            [void]$options.AppendChild($setting)
        }
        $setting.InnerText = 'false'
    }
    $vcx.Save($found.FullName)
    Write-Host "Disabled automatic STL modules for $projectName (C++23 remains enabled)."
}

& cmake --build $build --config Release --target FilledJarMarkerDetector
if ($LASTEXITCODE -ne 0) { throw 'C++ build failed; see the error above' }

$dll = Get-ChildItem -Path $build -Recurse -File -Filter 'FilledJarMarkerDetector.dll' | Select-Object -First 1
if (-not $dll) { throw 'Build reported success but DLL could not be located' }
# On supported MSVC developer shells, verify the resulting DLL does not still import
# the two vcpkg dynamic logging libraries. Missing dumpbin is not fatal.
if (Get-Command 'dumpbin.exe' -ErrorAction SilentlyContinue) {
    $imports = (& dumpbin.exe /DEPENDENTS $dll.FullName) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw 'dumpbin /DEPENDENTS failed' }
    if ($imports -match '(?im)^\s*(spdlog|fmt)\.dll\s*$') {
        throw 'Built DLL still imports spdlog.dll or fmt.dll; static linking was not applied.'
    }
    Write-Host 'Verified: no spdlog.dll or fmt.dll import dependency.'
}
$installDir = Join-Path $root 'install\SKSE\Plugins'
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $installDir $dll.Name) -Force
Copy-Item -LiteralPath (Join-Path $root 'SKSE\Plugins\FilledJarVisualTest.ini') -Destination (Join-Path $installDir 'FilledJarVisualTest.ini') -Force
$shaderInstall = Join-Path $installDir 'FilledJar'
New-Item -ItemType Directory -Force -Path $shaderInstall | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'SKSE\Plugins\FilledJar\FilledJarLiquid.hlsl') -Destination (Join-Path $shaderInstall 'FilledJarLiquid.hlsl') -Force
Write-Host "SUCCESS: $installDir\$($dll.Name)"
Write-Host 'Install this DLL IN PLACE of the previous version; keep a backup of your working marker tracer.'
Write-Host 'Install the entire install/SKSE/Plugins folder: DLL, INI and FilledJar/FilledJarLiquid.hlsl.'
