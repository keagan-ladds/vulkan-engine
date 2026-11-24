# Runs CMake for the MSVC build (used by .vscode/tasks.json "Configure / Build MSVC ...").
#
# Prefers the CMake bundled with the newest Visual Studio installed (found with
# Microsoft's vswhere): it always knows that Visual Studio's generator, e.g.
# "Visual Studio 18 2026", which an older cmake on PATH (w64devkit's) may not.
# Falls back to the cmake on PATH. Arguments are passed through:
#   powershell -File tools/vscode/msvc-cmake.ps1 --preset msvc
#   powershell -File tools/vscode/msvc-cmake.ps1 --build --preset msvc-debug
$ErrorActionPreference = 'Stop'

$cmake = 'cmake'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    # The newest install with the C++ toolset (-prerelease: Insiders / Preview too).
    $vs = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    if ($vs) {
        $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { $cmake = $bundled }
        Write-Host "Visual Studio: $vs"
    } else {
        Write-Host 'vswhere found no Visual Studio with the C++ toolset ("Desktop development with C++" workload).'
    }
}
# A CMAKE_GENERATOR set for the MinGW / Ninja build would override the
# preset's Visual Studio generator discovery.
Remove-Item Env:CMAKE_GENERATOR -ErrorAction SilentlyContinue
Write-Host "CMake: $cmake"
& $cmake @args
exit $LASTEXITCODE
