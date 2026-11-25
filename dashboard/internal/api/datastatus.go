package api

import (
	"encoding/json"
	"net"
	"net/http"
	"net/url"
	"os"
	"sort"
	"strings"
	"time"
)

// statusPath is where the renderer writes the health of each data
// source (renderer/datastatus.c). A variable so tests can point it
// elsewhere.
var statusPath = "/tmp/pi-clock-status.json"

// Friendly names for the hosts the renderer talks to.
var sourceNames = map[string]string{
	"celestrak.org":                  "CelesTrak · satellite orbits (Starlink trains, brightest satellites)",
	"db.satnogs.org":                 "SatNOGS · satellite orbits",
	"www.amsat.org":                  "AMSAT · amateur satellite orbits",
	"tle.ivanstanojevic.me":          "TLE API · ISS / Hubble / Tiangong orbits",
	"ll.thespacedevs.com":            "Launch Library · rocket launches",
	"gibs.earthdata.nasa.gov":        "NASA GIBS · satellite imagery, season map, sea temperature",
	"services.swpc.noaa.gov":         "NOAA SWPC · space weather, aurora",
	"api.weather.gov":                "National Weather Service · alerts",
	"earthquake.usgs.gov":            "USGS · earthquakes",
	"ws1.blitzortung.org":            "Blitzortung · lightning (live)",
	"map.blitzortung.org":            "Blitzortung · lightning",
	"stream.aisstream.io":            "AISstream · ships (live)",
	"v2-api-public.astrospheric.com": "Astrospheric · sky forecast",
	"api.ebird.org":                  "eBird · rare birds",
	"api.open-meteo.com":             "Open-Meteo · weather",
	"air-quality-api.open-meteo.com": "Open-Meteo · air quality",
	"api.adsb.lol":                   "adsb.lol · aircraft",
	"api.adsbdb.com":                 "adsbdb · aircraft routes",
	"api.tidesandcurrents.noaa.gov":  "NOAA · tides",
	"api.water.noaa.gov":             "NOAA · river gauges",
	"www.ndbc.noaa.gov":              "NOAA NDBC · buoys",
	"mesonet.agron.iastate.edu":      "Iowa Mesonet · radar, reports",
	"www.spc.noaa.gov":               "Storm Prediction Center · outlooks",
	"droughtmonitor.unl.edu":         "US Drought Monitor",
	"firms.modaps.eosdis.nasa.gov":   "NASA FIRMS · fires",
	"volcano.si.edu":                 "Smithsonian · volcanoes",
	"www.gdacs.org":                  "GDACS · tropical storms",
	"eyes.nasa.gov":                  "NASA DSN Now",
	"sdo.gsfc.nasa.gov":              "NASA SDO · sun images",
	"satepsanone.nesdis.noaa.gov":    "NOAA · smoke",
	"www.country-files.com":          "Country files · DX",
}

type sourceStatus struct {
	Host      string   `json:"host"`
	Users     []string `json:"users"`
	LastOK    int64    `json:"last_ok"`
	LastFail  int64    `json:"last_fail"`
	FailSince int64    `json:"fail_since"`
	Streak    int      `json:"streak"`
	OK        int      `json:"ok"`
	Fail      int      `json:"fail"`
	Error     string   `json:"error"`
	Name      string   `json:"name"`
	State     string   `json:"state"` // ok, failing, down
}

type statusFile struct {
	Updated int64          `json:"updated"`
	Sources []sourceStatus `json:"sources"`
}

// hostOf returns the host part of a URL setting, "" if none.
func hostOf(raw string) string {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return ""
	}
	h := u.Hostname()
	return h
}

// GetDataStatus reports, for every data source the renderer uses, when
// it last worked and whether it is failing now.
func GetDataStatus() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := os.ReadFile(statusPath)
		if err != nil {
			jsonResponse(w, http.StatusOK, map[string]any{"available": false,
				"now": time.Now().Unix()})
			return
		}
		var st statusFile
		if json.Unmarshal(data, &st) != nil {
			jsonResponse(w, http.StatusOK, map[string]any{"available": false,
				"now": time.Now().Unix()})
			return
		}
		kv, _ := readKV(rendererConfPath)
		pcHost := hostOf(kv["PC_HELPER_URL"])
		for i := range st.Sources {
			s := &st.Sources[i]
			s.Name = sourceNames[s.Host]
			if s.Name == "" {
				switch {
				case pcHost != "" && s.Host == pcHost:
					s.Name = "Windows PC helper"
				case net.ParseIP(s.Host) != nil && net.ParseIP(s.Host).IsPrivate():
					s.Name = "Home network device (e.g. Home Assistant)"
				default:
					s.Name = s.Host
				}
			}
			switch {
			case s.Streak == 0:
				s.State = "ok"
			case s.LastOK == 0 || s.Streak >= 5:
				s.State = "down"
			default:
				s.State = "failing"
			}
		}
		// Problems first, then by name
		rank := map[string]int{"down": 0, "failing": 1, "ok": 2}
		sort.SliceStable(st.Sources, func(a, b int) bool {
			ra, rb := rank[st.Sources[a].State], rank[st.Sources[b].State]
			if ra != rb {
				return ra < rb
			}
			return st.Sources[a].Name < st.Sources[b].Name
		})
		jsonResponse(w, http.StatusOK, map[string]any{"available": true,
			"now": time.Now().Unix(), "updated": st.Updated, "sources": st.Sources})
	}
}
