[CmdletBinding()]
param(
    [string]$BuildDirectory = '.\\build-gpio-pin-test'
)

$ErrorActionPreference = 'Stop'
$pluginRoot = 'C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins'
$cmake = Get-ChildItem "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.cmake.win32_*\tools\bin\cmake.exe" |
    Select-Object -First 1 -ExpandProperty FullName
$ninja = Get-ChildItem "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.ninja.win32_*\tools\bin\ninja.exe" |
    Select-Object -First 1 -ExpandProperty FullName
$gcc = Get-ChildItem "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin\arm-none-eabi-gcc.exe" |
    Select-Object -First 1 -ExpandProperty FullName
if (!$cmake -or !$ninja -or !$gcc) { throw 'STM32CubeIDE CMake, Ninja, or ARM GCC tool was not found.' }

$savedPath = $env:PATH
try {
    $env:PATH = ((Split-Path $cmake), (Split-Path $ninja), (Split-Path $gcc), 'C:\msys64\mingw64\bin', 'C:\Program Files\Git\bin' -join ';') + ';' + $savedPath
    $sdkPath = if ($env:PICO_SDK_PATH) { $env:PICO_SDK_PATH } else { Join-Path $env:USERPROFILE 'pico\pico-sdk' }
    $buildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)

    & $cmake -S (Join-Path $PSScriptRoot 'gpio-pin-test') -B $buildDir -G Ninja '-DPICO_BOARD=pico2_w' "-DPICO_SDK_PATH=$sdkPath" '-DCMAKE_BUILD_TYPE=Release'
    if ($LASTEXITCODE) { throw 'GPIO test configure failed.' }
    & $cmake --build $buildDir --parallel 4
    if ($LASTEXITCODE) { throw 'GPIO test build failed.' }
    Write-Host "GPIO test UF2: $buildDir\gpio_pin_test.uf2"
} finally {
    $env:PATH = $savedPath
}
