# Offline test of the installer's settings step: the port the firewall
# rule and probe use must be the port the service will read.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\helper-config.ps1")
$fail = 0
function Check($cond, $what) {
    if ($cond) { Write-Host "ok   $what" } else { Write-Host "FAIL $what"; $script:fail++ }
}
$dir = Join-Path ([IO.Path]::GetTempPath()) ("pch-" + [guid]::NewGuid())
New-Item -ItemType Directory $dir | Out-Null
$json = Join-Path $dir "helper.json"
try {
    # first install, defaults
    $p = Resolve-HelperConfig -Path $json -Lat 35.69 -Lon -78.73 -Radar KRAX -Port 8765 -PortGiven $false
    Check ($p -eq 8765) "new install uses the default port"
    Check ((Get-Content $json -Raw | ConvertFrom-Json).port -eq 8765) "new install writes the port"

    # the user moved the service to 9000 in helper.json; rerun with no arguments
    $c = Get-Content $json -Raw | ConvertFrom-Json; $c.port = 9000
    $c | ConvertTo-Json | Set-Content $json -Encoding ASCII
    $p = Resolve-HelperConfig -Path $json -Lat 35.69 -Lon -78.73 -Radar KRAX -Port 8765 -PortGiven $false
    Check ($p -eq 9000) "rerun without -Port keeps the saved port"
    Check ((Get-Content $json -Raw | ConvertFrom-Json).port -eq 9000) "saved port left alone"

    # rerun with an explicit -Port: the service must move too
    $p = Resolve-HelperConfig -Path $json -Lat 35.69 -Lon -78.73 -Radar KRAX -Port 8800 -PortGiven $true
    $c = Get-Content $json -Raw | ConvertFrom-Json
    Check ($p -eq 8800 -and $c.port -eq 8800) "explicit -Port is saved for the service"
    Check ($c.radar -eq "KRAX" -and $c.home_lat -eq 35.69) "other settings kept"

    # other settings survive a rerun with -Port: text with accents, and a
    # list (Windows PowerShell 5.1 can rewrite arrays as {"value": ...})
    $e = [char]0xEB    # an accented letter, kept out of this file's own text
    [IO.File]::WriteAllText($json, ('{"port": 8765, "cache_dir": "C:\\Users\\Zo' + $e + '\\cache", "jobs": ["nowcast", "birds"]}'),
        (New-Object Text.UTF8Encoding $false))
    $p = Resolve-HelperConfig -Path $json -Lat 35.69 -Lon -78.73 -Radar KRAX -Port 8800 -PortGiven $true
    $raw = [IO.File]::ReadAllText($json, [Text.Encoding]::UTF8)
    $c = $raw | ConvertFrom-Json
    Check ($c.cache_dir -eq "C:\Users\Zo$([char]0xEB)\cache") "accented path kept"
    Check ($c.jobs -is [array] -and $c.jobs.Count -eq 2 -and $c.jobs[1] -eq "birds") "list kept as a list"
    Check ($raw -notmatch '"value"') "no array wrapper"
    Check ([IO.File]::ReadAllBytes($json)[0] -ne 0xEF) "no byte-order mark"

    # a helper.json without a port uses the service's default
    '{"home_lat": 35.69, "home_lon": -78.73, "radar": "KRAX"}' | Set-Content $json -Encoding ASCII
    $p = Resolve-HelperConfig -Path $json -Lat 35.69 -Lon -78.73 -Radar KRAX -Port 8765 -PortGiven $false
    Check ($p -eq 8765) "no saved port: the service default"
} finally {
    Remove-Item -Recurse -Force $dir
}
if ($fail) { exit 1 }
