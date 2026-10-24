[CmdletBinding()]
param(
    [string]$Port,
    [ValidateRange(3, 30)][int]$Seconds = 8
)
$ErrorActionPreference = 'Stop'
# Reads the repeating USB diagnostic report. Sends no programming command and
# never resets/erases either board. START remains controlled by GP18 only.
if (-not $Port) {
    $candidates = @(Get-CimInstance Win32_SerialPort | Where-Object {
        $_.PNPDeviceID -match 'VID_2E8A'
    })
    if ($candidates.Count -ne 1) {
        throw 'Connect exactly one Pico running a standalone programmer UF2, or specify -Port COMx.'
    }
    $Port = $candidates[0].DeviceID
}
$serial = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,One
$serial.ReadTimeout = 500
$serial.DtrEnable = $true
$serial.RtsEnable = $false
$received = $false
try {
    $serial.Open()
    Write-Host "Reading $Port for $Seconds seconds. No flash command is sent."
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $line = $serial.ReadLine().Trim()
            if ($line) {
                Write-Output $line
                if ($line -match 'PICO-PROGRAMMER (recovery-v1|fast-v2|fast-v3|network-v[0-9]+|local-ap-v[0-9]+)') { $received = $true }
            }
        } catch [System.TimeoutException] {
            # A busy programming job publishes its result only after it finishes.
        }
    }
    if (-not $received) {
        Write-Warning 'No complete programmer report received. Let programming finish and read again without unplugging the Pico.'
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
