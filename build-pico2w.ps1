# Rebuild the existing Pico 2 W build using this laptop's installed tools.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$pluginRoot = 'C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins'
$toolPatterns = @(
    "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.cmake.win32_*\tools\bin\cmake.exe",
    "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.ninja.win32_*\tools\bin\ninja.exe",
    "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin\arm-none-eabi-gcc.exe"
)
$buildTools = foreach ($pattern in $toolPatterns) {
    $toolPath = Get-ChildItem -Path $pattern | Select-Object -First 1 -ExpandProperty FullName
    if (-not $toolPath) { throw "Required build tool not found: $pattern" }
    $toolPath
}
$buildDir = Join-Path $PSScriptRoot 'build-pico2w'
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
    throw 'Configure build-pico2w with CMake first; see README.md.'
}
$savedPath = $env:PATH
try {
    $toolDirs = @($buildTools | ForEach-Object { Split-Path $_ })
    $toolDirs += @('C:\msys64\mingw64\bin', 'C:\Program Files\Git\bin')
    $env:PATH = ($toolDirs -join ';') + ';' + $savedPath
    & $buildTools[0] --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "Pico build failed (exit code $LASTEXITCODE)." }
    Write-Host "UF2: $buildDir\debugprobe_on_pico2_w.uf2"
} finally {
    $env:PATH = $savedPath
}

    [int]$SpeedKHz = 4000,

