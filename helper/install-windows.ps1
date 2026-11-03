# Pi-Clock PC helper - Windows installer
#
# Right-click > "Run with PowerShell", or from a PowerShell window:
#   powershell -ExecutionPolicy Bypass -File .\install-windows.ps1
#
# It asks for administrator rights (for the startup task and the firewall
# rule), then:
#   1. installs Python 3.12 (winget) if it isn't there
#   2. copies the helper to C:\PiClockHelper and creates a virtual env
#      with its libraries (CPU PyTorch; MistNet doesn't need the GPU)
#   3. downloads the MistNet bird model (~540 MB, once)
#   4. registers a startup task "Pi-Clock Helper" (runs as SYSTEM, below
#      normal priority, restarts if it stops) and starts it
#   5. allows TCP 8765 in from the local network only
# Re-running it updates the helper in place. uninstall-windows.ps1 removes
# the task and firewall rule.

param([double]$Lat = 35.6928, [double]$Lon = -78.7270, [string]$Radar = "KRAX",
      [int]$Port = 8765)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"     # makes Invoke-WebRequest fast

# --- elevate ----------------------------------------------------------------
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Asking for administrator rights..."
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Lat $Lat -Lon $Lon -Radar $Radar -Port $Port"
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit
}

$Src = Split-Path -Parent $PSCommandPath
$Dest = "C:\PiClockHelper"
$TaskName = "Pi-Clock Helper"

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Fail($t) { Write-Host $t -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }

try {
    # --- 1. Python ----------------------------------------------------------
    Step "Python 3.12"
    $py = $null
    try { $py = (& py -3.12 -c "import sys; print(sys.executable)" 2>$null) } catch {}
    if (-not $py) {
        Write-Host "Installing Python 3.12 with winget..."
        winget install -e --id Python.Python.3.12 --scope machine --silent `
            --accept-package-agreements --accept-source-agreements | Out-Host
        $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                    [Environment]::GetEnvironmentVariable("Path", "User")
        foreach ($c in @("$env:ProgramFiles\Python312\python.exe",
                         "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe")) {
            if (Test-Path $c) { $py = $c; break }
        }
        if (-not $py) { try { $py = (& py -3.12 -c "import sys; print(sys.executable)") } catch {} }
    }
    if (-not $py) { Fail "Python 3.12 could not be installed. Install it from python.org and run this again." }
    Write-Host "Using $py"

    # --- 2. Files and libraries -------------------------------------------------
    Step "Copying the helper to $Dest"
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Start-Sleep 2
    }
    New-Item -ItemType Directory -Force -Path $Dest, "$Dest\models" | Out-Null
    Copy-Item -Recurse -Force "$Src\pchelper" $Dest
    Copy-Item -Force "$Src\requirements.txt", "$Src\uninstall-windows.ps1" $Dest
    if (-not (Test-Path "$Dest\helper.json")) {
        $cfg = @{ home_lat = $Lat; home_lon = $Lon; radar = $Radar; port = $Port } | ConvertTo-Json
        Set-Content -Path "$Dest\helper.json" -Value $cfg -Encoding ASCII
    }

    Step "Python libraries (a few minutes the first time)"
    if (-not (Test-Path "$Dest\venv\Scripts\python.exe")) { & $py -m venv "$Dest\venv" }
    $vpy = "$Dest\venv\Scripts\python.exe"
    & $vpy -m pip install --upgrade pip --quiet
    & $vpy -m pip install -r "$Dest\requirements.txt" --quiet
    if ($LASTEXITCODE -ne 0) { Fail "pip install failed (see above)." }
    & $vpy -m pip install torch --index-url https://download.pytorch.org/whl/cpu --quiet
    if ($LASTEXITCODE -ne 0) { Fail "PyTorch install failed (see above)." }
    & $vpy -c "import eccodes, cv2, pyart, torch, scipy; eccodes.codes_get_api_version(); print('libraries OK')"
    if ($LASTEXITCODE -ne 0) { Fail "A library failed to load (see above)." }

    # --- 3. MistNet model ------------------------------------------------------
    Step "MistNet bird/rain model"
    $model = "$Dest\models\mistnet_nexrad.pt"
    if (-not (Test-Path $model) -or (Get-Item $model).Length -lt 500MB) {
        Write-Host "Downloading (~540 MB)..."
        & curl.exe -L --fail -o "$model.part" "https://mistnet.s3.amazonaws.com/mistnet_nexrad.pt"
        if ($LASTEXITCODE -ne 0) { Fail "Model download failed; run this again to retry." }
        Move-Item -Force "$model.part" $model
    } else { Write-Host "Already downloaded." }

    # --- 4. Startup task ----------------------------------------------------------
    Step "Startup task '$TaskName'"
    $action = New-ScheduledTaskAction -Execute $vpy `
        -Argument "-m pchelper.server --config `"$Dest\helper.json`"" -WorkingDirectory $Dest
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName

    # --- 5. Firewall -----------------------------------------------------------
    Step "Firewall: TCP $Port from the local network only"
    Get-NetFirewallRule -DisplayName $TaskName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule -DisplayName $TaskName -Direction Inbound -Action Allow -Protocol TCP `
        -LocalPort $Port -RemoteAddress LocalSubnet -Profile Any | Out-Null

    # --- Done ---------------------------------------------------------------------
    Start-Sleep 8
    $ips = Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" -and
                       $_.PrefixOrigin -ne "WellKnown" } | Select-Object -ExpandProperty IPAddress
    $ok = $false
    try { $s = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/status.json" -TimeoutSec 10; $ok = $true } catch {}
    Write-Host ""
    if ($ok) { Write-Host "The helper is running." -ForegroundColor Green }
    else { Write-Host "Installed; the helper is still starting (check $Dest\helper.log)." -ForegroundColor Yellow }
    Write-Host "In the Pi-Clock dashboard, set 'PC helper address' to:"
    foreach ($ip in $ips) { Write-Host "    http://${ip}:$Port" -ForegroundColor Green }
    Write-Host ""
    Write-Host "Keep this PC from sleeping (Settings > System > Power) for the layers to stay current."
} catch {
    Fail ("Install failed: " + $_.Exception.Message)
}
Read-Host "Press Enter to close"
