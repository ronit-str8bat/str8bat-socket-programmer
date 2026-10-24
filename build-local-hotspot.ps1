[CmdletBinding()]
param(
    [string]$FirmwarePath = 'D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin',
    [string]$FirmwareVersion = 'v4.0.2',
    [string]$BuildDirectory = '',
    [switch]$RunTargetAfterVerify,
    [switch]$RgbCommonAnode,
    [switch]$StatusLedCommonAnode,
    [switch]$FactoryMaster,
    [switch]$FactorySlave,
    [string]$FactoryGroupKey = ''
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot\build-standalone.ps1" -FirmwarePath $FirmwarePath -FirmwareVersion $FirmwareVersion -BuildDirectory $BuildDirectory -LocalHotspot -HaltAfterVerify:(-not $RunTargetAfterVerify) -RgbCommonAnode:$RgbCommonAnode -StatusLedCommonAnode:$StatusLedCommonAnode -FactoryMaster:$FactoryMaster -FactorySlave:$FactorySlave -FactoryGroupKey $FactoryGroupKey
if ($LASTEXITCODE) { throw 'Local hotspot programmer build failed' }
