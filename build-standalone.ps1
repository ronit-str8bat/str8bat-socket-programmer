[CmdletBinding()]
param(
    [string]$FirmwarePath = 'D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin',
    [switch]$HaltAfterVerify,
    [switch]$Networking,
    [switch]$LocalHotspot,
    [switch]$RgbCommonAnode,
    [switch]$StatusLedCommonAnode,
    [switch]$FactoryMaster,
    [switch]$FactorySlave,
    [string]$FactoryGroupKey = '',
    [string]$FirmwareVersion = 'v4.0.2',
    [string]$BuildDirectory = ''
)
$ErrorActionPreference = 'Stop'
$pluginRoot = 'C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins'
$patterns = @(
    "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.cmake.win32_*\tools\bin\cmake.exe",
    "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.ninja.win32_*\tools\bin\ninja.exe",
    "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*\tools\bin\arm-none-eabi-gcc.exe"
)
$buildTools = foreach ($pattern in $patterns) {
    $tool = Get-ChildItem $pattern | Select-Object -First 1 -ExpandProperty FullName
    if (-not $tool) { throw "Missing build tool: $pattern" }
    $tool
}
$image = (Get-Item -LiteralPath $FirmwarePath).FullName
$pythonExe = (Get-Command python -ErrorAction Stop).Source
$savedPath = $env:PATH
try {
    $toolDirs = @($buildTools | ForEach-Object { Split-Path $_ })
    $toolDirs += @('C:\msys64\mingw64\bin', 'C:\Program Files\Git\bin')
    $env:PATH = ($toolDirs -join ';') + ';' + $savedPath
    $sdkPath = Join-Path $env:USERPROFILE 'pico\pico-sdk'
    if ($env:PICO_SDK_PATH) { $sdkPath = $env:PICO_SDK_PATH }
    if ($LocalHotspot) { $Networking = $true }
    $buildFolder = if ($LocalHotspot) { 'build-local-hotspot' } elseif ($Networking) { 'build-network' } else { 'build-standalone' }
    $buildDir = if ($BuildDirectory) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory) } else { Join-Path $PSScriptRoot $buildFolder }
    # CMake/Ninja can repeatedly regenerate when a full drive prevents saving
    # the new build files. Fail early and allow output on a different drive.
    $buildDrive = [System.IO.DriveInfo]::new([System.IO.Path]::GetPathRoot($buildDir))
    if ($buildDrive.AvailableFreeSpace -lt 100MB) {
        throw "Not enough free space on $($buildDrive.Name). Free at least 100 MB or pass -BuildDirectory with a folder on another drive."
    }
    $haltSetting = if ($HaltAfterVerify) { 'ON' } else { 'OFF' }
    $networkSetting = if ($Networking) { 'ON' } else { 'OFF' }
    $localSetting = if ($LocalHotspot) { 'ON' } else { 'OFF' }
    $rgbCommonAnodeSetting = if ($RgbCommonAnode) { 'ON' } else { 'OFF' }
    $statusLedCommonAnodeSetting = if ($StatusLedCommonAnode) { 'ON' } else { 'OFF' }
    if ($FactoryMaster -and $FactorySlave) { throw 'Choose only one of -FactoryMaster or -FactorySlave.' }
    if (($FactoryMaster -or $FactorySlave) -and $FactoryGroupKey -notmatch '^[A-Za-z0-9._-]{16,63}$') {
        throw 'FactoryGroupKey must be 16..63 letters, digits, dot, underscore or hyphen.'
    }
    $factoryMasterSetting = if ($FactoryMaster) { 'ON' } else { 'OFF' }
    $factorySlaveSetting = if ($FactorySlave) { 'ON' } else { 'OFF' }
    & $buildTools[0] -S (Join-Path $PSScriptRoot 'standalone') -B $buildDir -G Ninja '-DPICO_BOARD=pico2_w' "-DPICO_SDK_PATH=$sdkPath" "-DTARGET_FIRMWARE=$image" "-DPython3_EXECUTABLE=$pythonExe" '-DCMAKE_BUILD_TYPE=Release' "-DSTANDALONE_HALT_AFTER_VERIFY=$haltSetting" "-DSTANDALONE_NETWORK=$networkSetting" "-DSTANDALONE_LOCAL_AP=$localSetting" "-DSTANDALONE_RGB_COMMON_ANODE=$rgbCommonAnodeSetting" "-DSTANDALONE_STATUS_LED_COMMON_ANODE=$statusLedCommonAnodeSetting" "-DSTANDALONE_FACTORY_MASTER=$factoryMasterSetting" "-DSTANDALONE_FACTORY_SLAVE=$factorySlaveSetting" "-DFACTORY_GROUP_KEY=$FactoryGroupKey" "-DTARGET_FIRMWARE_VERSION=$FirmwareVersion"
    if ($LASTEXITCODE) { throw 'Standalone configure failed' }
    & $buildTools[0] --build $buildDir --parallel 4
    if ($LASTEXITCODE) { throw 'Standalone build failed' }
    Write-Host "Standalone UF2: $buildDir\standalone_programmer.uf2"
    if ($HaltAfterVerify) { Write-Host 'Diagnostic build: green means verified and target left halted; no application restart.' }
} finally { $env:PATH = $savedPath }
