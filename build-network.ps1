[CmdletBinding()]
param(
    [string]$FirmwarePath = 'D:\shashank\Firmware_bin\production_firmware_v4.0.2.bin',
    [switch]$RunTargetAfterVerify
)
$ErrorActionPreference = 'Stop'
# Preserve the earlier standalone build directory and named, tested UF2s.
& "$PSScriptRoot\build-standalone.ps1" -FirmwarePath $FirmwarePath -Networking -HaltAfterVerify:(-not $RunTargetAfterVerify)
if ($LASTEXITCODE) { throw 'Network programmer build failed' }
