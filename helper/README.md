# Pi-Clock PC helper

Runs the jobs a Raspberry Pi can't, on a PC on the same network, and
serves the results to the clock:

| Layer on the clock | Job | Data | Refresh |
|---|---|---|---|
| Rain Arriving | `nowcast` | MRMS radar composite (NOAA, 1 km, 2-min) extrapolated 90 min ahead with optical flow | 5 min |
| Cloud / Smoke / Wind Gust Forecast, Frost Tonight | `hrrr` | NOAA HRRR 3 km model, next 18 h (only the 4 needed fields, via byte ranges: ~60 MB per run) | 3 h |
| Bird Migration Tonight | `birds` | KRAX NEXRAD Level II + MistNet (Lin et al. 2019) rain/bird segmentation, vol2bird-style density and VAD profile | each scan at night |

Each layer also carries a note about home ("Rain reaching home ~2:40 PM
(in 35 min)", "Tonight at home: clear 9 PM–2 AM", "~1,200 birds/km/h
heading SW").

## Install (Windows)

1. Copy this `helper` folder to the PC.
2. Right-click `install-windows.ps1` > **Run with PowerShell** (it asks for
   administrator rights). It installs Python 3.12 if needed, the libraries,
   the MistNet model (~540 MB), a startup task and a firewall rule for the
   local network, then prints the address to enter in the dashboard's
   **PC Helper** card.
3. Turn on the "PC helper" layers in the dashboard.

Options: `install-windows.ps1 -Lat 35.69 -Lon -78.73 -Radar KRAX -Port 8765`.
Settings live in `C:\PiClockHelper\helper.json`, the log in
`C:\PiClockHelper\helper.log`; `http://<pc>:8765/status.json` shows each
job's state. `uninstall-windows.ps1` removes the task and firewall rule.

The PC must be awake for the layers to stay current; when it isn't, the
clock keeps the last frames while valid and then hides the layers.

## Run anywhere else (Linux/macOS)

    python3 -m venv venv && . venv/bin/activate
    pip install -r requirements.txt torch
    curl -Lo models/mistnet_nexrad.pt https://mistnet.s3.amazonaws.com/mistnet_nexrad.pt
    python -m pchelper.server --config helper.json      # --once nowcast to test one job

## Output format

`<product>/manifest.json` plus one file per frame: raw 8-bit palette
indices, width x height, row 0 at the north edge of `bbox`
(lon0, lat0, lon1, lat1). Index 0 is transparent; `palette` holds RGBA.
`note` may contain `{t:EPOCH}` and `{in:EPOCH}`, which the clock formats
as local time and "in 35 min".
