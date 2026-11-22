# Settings step of install-windows.ps1, kept apart so it can be tested.
#
# Returns the port the helper service will actually listen on, so the
# firewall rule, the probe and the printed address all match it:
#   - a new install writes helper.json from the arguments;
#   - a rerun keeps the saved helper.json, and its port, unless -Port was
#     given, in which case that port is saved so the service moves too.

function Resolve-HelperConfig {
    param([string]$Path, [double]$Lat, [double]$Lon, [string]$Radar,
          [int]$Port, [bool]$PortGiven)
    if (-not (Test-Path $Path)) {
        @{ home_lat = $Lat; home_lon = $Lon; radar = $Radar; port = $Port } |
            ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
        return $Port
    }
    $cfg = Get-Content -Raw -Path $Path | ConvertFrom-Json
    if ($PortGiven) {
        $cfg | Add-Member -NotePropertyName port -NotePropertyValue $Port -Force
        $cfg | ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
        return $Port
    }
    if ($null -ne $cfg.port) { return [int]$cfg.port }
    return 8765      # the service's own default (pchelper/common.py)
}
