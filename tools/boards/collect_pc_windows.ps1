# Read-only Windows hardware inventory for physical PC ports (PowerShell 5.1+).
# No disk writes except the explicitly named report; no registry/firmware changes.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$issues = New-Object 'System.Collections.Generic.List[string]'

function Read-CimInventory {
    param([string]$ClassName, [string[]]$Fields, [string]$Filter = '')
    try {
        $query = @{ ClassName = $ClassName; ErrorAction = 'Stop' }
        if ($Filter) { $query.Filter = $Filter }
        @(Get-CimInstance @query | Select-Object -Property $Fields)
    } catch {
        $issues.Add("${ClassName}: $($_.Exception.Message)")
    }
}

$firmware = 'Unknown'
try {
    # GetFirmwareType is available since Windows 8. Older systems retain Unknown.
    if (-not ('A20Inventory.Firmware' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
namespace A20Inventory {
    public static class Firmware {
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetFirmwareType(out uint firmwareType);
    }
}
'@
    }
    [uint32]$kind = 0
    if ([A20Inventory.Firmware]::GetFirmwareType([ref]$kind)) {
        $firmware = switch ($kind) { 1 { 'LegacyBIOS' }; 2 { 'UEFI' }; default { 'Unknown' } }
    } else {
        $issues.Add('GetFirmwareType failed; inspect msinfo32 BIOS Mode manually.')
    }
} catch {
    $issues.Add("Firmware interface: $($_.Exception.Message)")
}

# Exclude machine name, serial numbers, UUID, network addresses and product keys.
# Include complete hardware IDs: VEN/DEV alone cannot identify every MAC revision.
$report = [ordered]@{
    format = 'a20-pc-inventory-v1'
    collected_utc = [DateTime]::UtcNow.ToString('o')
    firmware_interface = $firmware
    firmware_bitness = 'Not determined by this report; OSArchitecture is not firmware bitness.'
    system = @(Read-CimInventory 'Win32_ComputerSystem' @('Manufacturer', 'Model', 'SystemType', 'TotalPhysicalMemory'))
    bios = @(Read-CimInventory 'Win32_BIOS' @('Manufacturer', 'SMBIOSBIOSVersion', 'ReleaseDate'))
    os = @(Read-CimInventory 'Win32_OperatingSystem' @('Caption', 'Version', 'OSArchitecture'))
    cpu = @(Read-CimInventory 'Win32_Processor' @('Name', 'Manufacturer', 'Architecture', 'NumberOfCores', 'NumberOfLogicalProcessors'))
    pci = @(Read-CimInventory 'Win32_PnPEntity' @('Name', 'PNPDeviceID', 'HardwareID', 'CompatibleID', 'Service', 'ConfigManagerErrorCode') "PNPDeviceID LIKE 'PCI%'")
    audio = @(Read-CimInventory 'Win32_PnPEntity' @('Name', 'PNPDeviceID', 'HardwareID', 'CompatibleID', 'Service') "PNPDeviceID LIKE 'HDAUDIO%'")
    serial = @(Read-CimInventory 'Win32_SerialPort' @('Name', 'DeviceID', 'PNPDeviceID'))
    display = @(Read-CimInventory 'Win32_VideoController' @('Name', 'PNPDeviceID', 'DriverVersion', 'CurrentHorizontalResolution', 'CurrentVerticalResolution'))
    issues = @($issues.ToArray())
}

$json = $report | ConvertTo-Json -Depth 8
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
# CreateNew refuses to replace an existing file; parent directory must exist.
$file = [System.IO.File]::Open($destination, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
try {
    $data = (New-Object System.Text.UTF8Encoding($false)).GetBytes($json + "`n")
    $file.Write($data, 0, $data.Length)
} finally {
    $file.Dispose()
}
Write-Output "Hardware report saved: $destination"
