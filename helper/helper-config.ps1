# Settings step of install-windows.ps1, kept apart so it can be tested.
#
# Returns the port the helper service will actually listen on, so the
# firewall rule, the probe and the printed address all match it:
#   - a new install writes helper.json from the arguments;
#   - a rerun keeps the saved helper.json, and its port, unless -Port was
#     given, in which case that port is saved so the service moves too.

# UTF-8 without a byte-order mark, so paths with accents survive and
# Python reads the file (Windows PowerShell's ASCII would turn them to "?")
function Save-HelperJson([string]$Path, [string]$Json) {
    [IO.File]::WriteAllText($Path, $Json, (New-Object Text.UTF8Encoding $false))
}

function Resolve-HelperConfig {
    param([string]$Path, [double]$Lat, [double]$Lon, [string]$Radar,
          [int]$Port, [bool]$PortGiven)
    if (-not (Test-Path $Path)) {
        Save-HelperJson $Path (@{ home_lat = $Lat; home_lon = $Lon; radar = $Radar; port = $Port } |
            ConvertTo-Json -Depth 8)
        return $Port
    }
    $cfg = Get-Content -Raw -Path $Path | ConvertFrom-Json
    if ($PortGiven) {
        $cfg | Add-Member -NotePropertyName port -NotePropertyValue $Port -Force
        Save-HelperJson $Path ($cfg | ConvertTo-Json -Depth 8)
        return $Port
    }
    if ($null -ne $cfg.port) { return [int]$cfg.port }
    return 8765      # the service's own default (pchelper/common.py)
}
