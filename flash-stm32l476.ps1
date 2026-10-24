# Without -FirmwarePath, checks the target connection without programming flash.
# A raw .bin requires its linked flash address; .elf/.hex contain addresses.
[CmdletBinding()]
param(
    [string]$FirmwarePath,
    [string]$Address,
    [ValidateRange(1, 10000)]
    [int]$SpeedKHz = 4000,
    # Requires Pico GP1 (physical pin 2) wired to the STM32 NRST signal.
    [switch]$ConnectUnderReset,
    [switch]$SoftwareReset,
    [switch]$VerifyOnly,
    [switch]$EraseOnly,
    [switch]$ClockInfo,
    [switch]$FastClock,
    [string]$DiagnosticLog
)

$ErrorActionPreference = 'Stop'
if ($FastClock -and (-not $SoftwareReset -or $VerifyOnly -or $EraseOnly)) {
    throw '-FastClock requires -SoftwareReset and supports programming or connection/clock checks only.'
}
if ($ClockInfo -and ($FirmwarePath -or $Address -or $VerifyOnly -or $EraseOnly)) {
    throw 'Use -ClockInfo without firmware, verify, or erase options.'
}
if ($EraseOnly -and ($FirmwarePath -or $Address -or $VerifyOnly)) {
    throw 'Use -EraseOnly without -FirmwarePath, -Address, or -VerifyOnly.'
}
if ($ConnectUnderReset -and $SoftwareReset) {
    throw 'Choose either -ConnectUnderReset or -SoftwareReset.'
}
if ($VerifyOnly -and -not $FirmwarePath) {
    throw '-VerifyOnly requires -FirmwarePath and a raw .bin image.'
}

$pluginRoot = 'C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins'
$openocd = Get-ChildItem "$pluginRoot\com.st.stm32cube.ide.mcu.externaltools.openocd.win32_*\tools\bin\openocd.exe" |
    Select-Object -First 1 -ExpandProperty FullName
$scripts = Get-ChildItem "$pluginRoot\com.st.stm32cube.ide.mcu.debug.openocd_*\resources\openocd\st_scripts" |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $openocd -or -not $scripts) {
    throw 'OpenOCD or its scripts were not found in this laptop''s STM32CubeIDE installation.'
}

$resetConfig = 'reset_config none'
$operation = 'init; flash probe 0; flash info 0; shutdown'
if ($ConnectUnderReset) {
    $resetConfig = 'reset_config srst_only srst_nogate srst_open_drain connect_assert_srst'
    $operation = 'init; reset halt; flash probe 0; flash info 0; $_TARGETNAME configure -event reset-deassert-post {}; reset run; shutdown'
    Write-Host 'Connecting under reset: GP1 must be wired to STM32 NRST. The target will restart.'
}
if ($SoftwareReset) {
    $resetConfig = 'reset_config none; cortex_m reset_config sysresetreq'
    $operation = 'init; reset halt; flash probe 0; flash info 0; $_TARGETNAME configure -event reset-deassert-post {}; reset run; shutdown'
    Write-Host 'Using Cortex-M system reset through SWD.'
}
if ($FirmwarePath) {
    $firmware = Get-Item -LiteralPath $FirmwarePath
    if ($firmware.PSIsContainer -or $firmware.Extension.ToLowerInvariant() -notin @('.bin', '.elf', '.hex')) {
        throw 'Select an STM32 .bin, .elf, or .hex firmware file.'
    }
    if ($firmware.Extension.ToLowerInvariant() -eq '.bin') {
        if ($Address -notmatch '^0x[0-9a-fA-F]+$') {
            throw 'For a .bin, supply its linked address using -Address 0x08000000 (or the actual application address).'
        }
    } elseif ($Address) {
        throw 'Omit -Address for .elf/.hex files; they contain their own addresses.'
    }

    $tclPath = $firmware.FullName.Replace('\', '/')
    # Braces quote spaces in Tcl; reject names that would break that quoting.
    if ($tclPath -match '[{}\r\n]') {
        throw 'Use a firmware path without braces or line breaks.'
    }
    $operation = "program {$tclPath} verify"
    if ($Address) { $operation += " $Address" }
    # Remove the explicit-halt hook before the final reset so the app runs.
    $operation += '; $_TARGETNAME configure -event reset-deassert-post {}; reset run; shutdown'
    if ($VerifyOnly) {
        if ($firmware.Extension.ToLowerInvariant() -ne '.bin') {
            throw '-VerifyOnly currently requires a raw .bin image.'
        }
        $readbackPath = [System.IO.Path]::GetTempFileName()
        $tclReadback = $readbackPath.Replace('\', '/')
        $operation = "init; reset halt; dump_image {$tclReadback} $Address $($firmware.Length); shutdown"
        Write-Host 'Readback verification only: no flash erase or write. Target stays halted.'
    } else {
        Write-Host "Programming STM32L476 with $($firmware.FullName)"
    }
} elseif ($Address) {
    throw '-Address requires -FirmwarePath.'
} elseif (-not $EraseOnly) {
    Write-Host 'Checking the STM32L476 connection; no flash erase or write is requested.'
}

if ($EraseOnly) {
    $operation = 'init; reset init; flash probe 0; stm32l4x mass_erase 0; shutdown'
    Write-Host 'Erasing all STM32 main flash, including firmware and stored data. Target stays halted.'
}

if ($DiagnosticLog -and $ConnectUnderReset -and -not $FirmwarePath -and -not $EraseOnly -and -not $ClockInfo) {
    # If reset/halt fails, read the physical reset input and core status before
    # OpenOCD disconnects. DAP_SWJ_Pins with a zero select mask only reads pins.
    $operation = @'
init
if {[catch {reset halt} reset_error]} {
    echo {NRST input (bit 7 of the first returned byte):}
    catch {cmsis-dap cmd 0x10 0 0 0 0 0 0}
    echo {DHCSR core status:}
    catch {read_memory 0xE000EDF0 32 1} status
    echo $status
    echo {DEMCR reset catch:}
    catch {read_memory 0xE000EDFC 32 1} status
    echo $status
    echo {RCC_CSR reset flags:}
    catch {read_memory 0x40021094 32 1} status
    echo $status
    echo {Trying an explicit halt after the reset timeout:}
    catch {halt 1000} status
    echo $status
    catch {poll} status
    echo $status
    error $reset_error
}
flash probe 0
echo [flash info 0]
echo {Flash registers ACR through WRP2BR:}
echo [read_memory 0x40022000 32 21]
echo {Core DHCSR:}
echo [read_memory 0xE000EDF0 32 1]
$_TARGETNAME configure -event reset-deassert-post {}
reset run
shutdown
'@
}

# CLOCK_FREQ also sets the speed used by this ST target script after reset.
if ($ClockInfo) {
    # Inspect the reset/halt state used for programming. Read registers only;
    # no clock configuration or flash writes, then restart the application.
    $operation = @'
init
reset halt
set cr [lindex [read_memory 0x40021000 32 1] 0]
set cfgr [lindex [read_memory 0x40021008 32 1] 0]
set pll [lindex [read_memory 0x4002100c 32 1] 0]
set csr [lindex [read_memory 0x40021094 32 1] 0]
set acr [lindex [read_memory 0x40022000 32 1] 0]
set pwr [lindex [read_memory 0x40007000 32 1] 0]
echo [format {RCC_CR=0x%08x RCC_CFGR=0x%08x PLLCFGR=0x%08x RCC_CSR=0x%08x} $cr $cfgr $pll $csr]
echo [format {FLASH_ACR=0x%08x PWR_CR1=0x%08x} $acr $pwr]
set source [expr {($cfgr >> 2) & 3}]
set range [expr {($cr & 8) ? (($cr >> 4) & 15) : (($csr >> 8) & 15)}]
set msi [lindex {100000 200000 400000 800000 1000000 2000000 4000000 8000000 16000000 24000000 32000000 48000000} $range]
set hz 0
if {$source == 0} {set hz $msi}
if {$source == 1} {set hz 16000000}
if {$source == 3} {
    set pll_source [expr {$pll & 3}]
    set input 0
    if {$pll_source == 1} {set input $msi}
    if {$pll_source == 2} {set input 16000000}
    if {$input > 0} {
        set hz [expr {($input * (($pll >> 8) & 127)) / (((($pll >> 4) & 7) + 1) * (2 * ((($pll >> 25) & 3) + 1)))}]
    }
}
set divisor [lindex {1 1 1 1 1 1 1 1 2 4 8 16 64 128 256 512} [expr {($cfgr >> 4) & 15}]]
echo [format {Clock source: %s; AHB divider: %d} [lindex {MSI HSI16 HSE PLL} $source] $divisor]
if {$hz > 0} {
    echo [format {Configured SYSCLK: %d Hz; CPU HCLK: %d Hz} $hz [expr {$hz / $divisor}]]
} else {
    echo {Frequency requires the board HSE frequency or a valid MSI range; inspect registers above.}
}
$_TARGETNAME configure -event reset-deassert-post {}
reset run
shutdown
'@
    Write-Host 'Reading the STM32 clock configuration after reset/halt; application restarts afterward.'
}
Write-Host "SWD speed: $SpeedKHz kHz"
$initialSpeed = $SpeedKHz
if ($FastClock) {
    $initialSpeed = [Math]::Min(500, $SpeedKHz)
    Write-Host "Temporary STM32 clock: MSI 24 MHz; initial SWD speed: $initialSpeed kHz."
    # Make checks exercise the same reset-init hook as programming.
    $operation = $operation.Replace('reset halt', 'reset init')
}
$resetInit = '$_TARGETNAME configure -event reset-init {}'
if (($FirmwarePath -and -not $VerifyOnly) -or $EraseOnly) {
    # Clear the observed stale PGAERR/PGSERR flags (RM0351: write one to clear)
    # after reset/halt, before the driver's pre-erase status check.
    $resetInit = '$_TARGETNAME configure -event reset-init { mww 0x40022010 0x000000a0 }'
}
$openocdArgs = @(
    '-s', $scripts,
    '-f', 'interface/cmsis-dap.cfg',
    '-c', 'transport select swd',
    '-c', "set CLOCK_FREQ $initialSpeed",
    '-c', "set CONNECT_UNDER_RESET $([int]$ConnectUnderReset.IsPresent)",
    # Keep debug access in low-power modes and freeze watchdogs while halted.
    '-c', 'set ENABLE_LOW_POWER 1',
    '-c', 'set STOP_WATCHDOG 1',
    '-f', 'target/stm32l4x.cfg',
    '-c', $resetConfig,
    # This target failed to halt on reset catch, but an explicit halt works.
    # Request halt after releasing reset; let OpenOCD wait outside the event.
    '-c', '$_TARGETNAME configure -event reset-deassert-post { halt }',
    # Preserve the target's clock and flash latency. The bundled reset-init
    # assumes pristine reset state, but this target needs an explicit halt.
    '-c', $resetInit,
    '-c', $operation
)
if ($DiagnosticLog) {
    $logPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DiagnosticLog).Replace('\', '/')
    $openocdArgs = @('-d3', '-l', $logPath) + $openocdArgs
    Write-Host "OpenOCD diagnostic log: $logPath"
}
if ($FastClock) {
    $clockScript = (Join-Path $PSScriptRoot 'stm32l476-fast-clock.cfg').Replace('\', '/')
    # Insert before the final operation, which starts OpenOCD execution.
    $openocdArgs = $openocdArgs[0..($openocdArgs.Count - 3)] + @(
        '-c', "set PROGRAM_SWD_KHZ $SpeedKHz",
        '-f', $clockScript,
        '-c', $operation
    )
}
& $openocd @openocdArgs
if ($LASTEXITCODE -ne 0) {
    if ($VerifyOnly) {
        Write-Host "Incomplete readback retained at $readbackPath"
    }
    throw "OpenOCD failed (exit code $LASTEXITCODE). Check the messages above."
}
if ($VerifyOnly) {
    $expectedHash = (Get-FileHash -LiteralPath $firmware.FullName -Algorithm SHA256).Hash
    $actualHash = (Get-FileHash -LiteralPath $readbackPath -Algorithm SHA256).Hash
    if ($expectedHash -ne $actualHash) {
        throw "Readback differs from firmware. Readback retained at $readbackPath"
    }
    Remove-Item -LiteralPath $readbackPath
    Write-Host 'Full readback SHA-256 matches firmware. Target remains halted.'
}
