# Runs CMake for the MSVC build (used by .vscode/tasks.json "Configure / Build MSVC ...").
#
# Uses the newest Visual Studio installed (found with Microsoft's vswhere):
#   - its bundled CMake, which always knows that Visual Studio's generator
#     ("Visual Studio 18 2026"), unlike an older cmake on PATH (w64devkit's);
#   - when configuring, that generator explicitly (-G), so the build uses its
#     own toolset (VS 2026: v145) and not an older Visual Studio's;
#   - a build-vs/ cache made with another generator (e.g. an earlier
#     "Visual Studio 17 2022" attempt) is cleared first: CMake can't switch
#     generators in place, and the cache is regenerated anyway.
# Arguments are passed through to CMake:
#   powershell -File tools/vscode/msvc-cmake.ps1 --preset msvc
#   powershell -File tools/vscode/msvc-cmake.ps1 --build --preset msvc-debug
$ErrorActionPreference = 'Stop'
$buildDir = 'build-vs' # the msvc preset's binaryDir

$cmake = 'cmake'
$generator = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    # The newest install with the C++ toolset (-prerelease: Insiders / Preview too).
    $vs = & $vswhere -latest -prerelease -products * -format json `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 | ConvertFrom-Json | Select-Object -First 1
    if ($vs) {
        $bundled = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { $cmake = $bundled }
        # CMake's generator name: "Visual Studio <major> <year>".
        $major = [int]($vs.installationVersion.Split('.')[0])
        $years = @{ 16 = '2019'; 17 = '2022'; 18 = '2026' }
        $year = $years[$major]
        if (-not $year -and $vs.catalog) { $year = $vs.catalog.productLineVersion }
        if ($year) { $generator = "Visual Studio $major $year" }
        Write-Host "Visual Studio: $($vs.installationPath) ($($vs.installationVersion))"
    } else {
        Write-Host 'vswhere found no Visual Studio with the C++ toolset ("Desktop development with C++" workload).'
    }
}
# A CMAKE_GENERATOR set for the MinGW / Ninja build would override the preset.
Remove-Item Env:CMAKE_GENERATOR -ErrorAction SilentlyContinue
Write-Host "CMake: $cmake"

$configuring = -not ($args -contains '--build')
if ($configuring -and $generator) {
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    if (Test-Path $cache) {
        $line = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
        $previous = if ($line) { $line.Matches[0].Groups[1].Value } else { '' }
        if ($previous -ne $generator) {
            Write-Host "build-vs was configured with '$previous': clearing its CMake cache for '$generator'"
            Remove-Item $cache -Force
            Remove-Item (Join-Path $buildDir 'CMakeFiles') -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host "Generator: $generator"
    & $cmake -G $generator @args
} else {
    & $cmake @args
}
exit $LASTEXITCODE
