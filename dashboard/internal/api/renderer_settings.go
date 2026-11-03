package api

// Generic renderer settings — one GET/PUT pair for simple KEY=value
// options in /data/etc/pi-clock-renderer.conf, so new layers don't each
// need their own handler pair. Only keys listed in rendererSettingDefs
// can be read or written, and every value is validated before saving.

import (
	"encoding/json"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"strings"

	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/config"
)

// rendererConfPath is a var so tests can point it at a temp file.
var rendererConfPath = "/data/etc/pi-clock-renderer.conf"

type rendererSetting struct {
	Default string
	Valid   func(string) bool
}

func boolSetting(def string) rendererSetting {
	return rendererSetting{def, func(v string) bool { return v == "0" || v == "1" }}
}

func intSetting(def string, min, max int64) rendererSetting {
	return rendererSetting{def, func(v string) bool {
		n, err := strconv.ParseInt(v, 10, 64)
		return err == nil && n >= min && n <= max
	}}
}

func enumSetting(def string, allowed ...string) rendererSetting {
	return rendererSetting{def, func(v string) bool {
		for _, a := range allowed {
			if v == a {
				return true
			}
		}
		return false
	}}
}

// hostPortRe accepts "host:port" or a bare host — used for optional
// local data sources such as an ADS-B receiver on the LAN.
var hostPortRe = regexp.MustCompile(`^[A-Za-z0-9.\-]{1,253}(:[0-9]{1,5})?$`)

// patternSetting accepts the empty string (use the default behaviour) or
// a value matching re.
func patternSetting(def string, re *regexp.Regexp) rendererSetting {
	return rendererSetting{def, func(v string) bool { return v == "" || re.MatchString(v) }}
}

var (
	noradListRe = regexp.MustCompile(`^[0-9]{1,6}(,[0-9]{1,6}){0,5}$`)
	tideIDRe    = regexp.MustCompile(`^[A-Za-z0-9]{3,12}$`)
	buoyListRe  = regexp.MustCompile(`^[A-Za-z0-9]{3,6}(,[A-Za-z0-9]{3,6}){0,3}$`)
	// Stored as "Name:lat:lon,Name:lat:lon" with "_" for spaces, so the
	// value is inert even if an old init script sources the file as
	// shell (see normalizeSetting). Max 10 sites, 400 characters.
	darkSitesRe = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,31}:-?[0-9]{1,2}(\.[0-9]{1,6})?:-?[0-9]{1,3}(\.[0-9]{1,6})?(,[A-Za-z0-9_.-]{1,31}:-?[0-9]{1,2}(\.[0-9]{1,6})?:-?[0-9]{1,3}(\.[0-9]{1,6})?){0,9}$`)
)

// normalizeSetting rewrites friendly input into the stored form before
// validation: dark-site names typed with spaces become "_" and ";"
// separators become ",". Everything stored in renderer.conf stays free
// of shell metacharacters.
func normalizeSetting(k, v string) string {
	if k != "DARK_SITES" {
		return v
	}
	parts := strings.FieldsFunc(v, func(r rune) bool { return r == ';' || r == ',' })
	out := parts[:0]
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		p = strings.Map(func(r rune) rune {
			switch r {
			case ' ':
				return '_'
			case '\'', '(', ')':
				return -1
			}
			return r
		}, p)
		out = append(out, p)
	}
	v = strings.Join(out, ",")
	if len(v) > 400 {
		return "!" // over the cap: fails validation
	}
	return v
}

func hostPortSetting() rendererSetting {
	return rendererSetting{"", func(v string) bool { return v == "" || hostPortRe.MatchString(v) }}
}

var rendererSettingDefs = map[string]rendererSetting{
	// Volcanoes
	"VOLCANO_LABELS":   boolSetting("1"),
	"VOLCANO_NEW_ONLY": boolSetting("0"),

	// Tropical cyclones
	"STORM_CONE":    boolSetting("1"),
	"STORM_TRACK":   boolSetting("1"),
	"STORM_MIN_CAT": intSetting("0", 0, 6),

	// NWS weather alerts
	"WX_BANNER":     boolSetting("1"),
	"WX_BANNER_MIN": intSetting("2", 1, 4),
	"WX_RADIUS_KM":  intSetting("0", 0, 5000),

	// Live radar
	"RADAR_TIME": boolSetting("1"),

	// Air quality
	"AQ_BADGE": boolSetting("1"),

	// Aircraft
	"AIRCRAFT_RADIUS_NM": intSetting("150", 25, 250),
	"AIRCRAFT_MIL":       boolSetting("0"),
	"AIRCRAFT_LABELS":    enumSetting("1", "0", "1", "2"),
	"AIRCRAFT_LOCAL":     hostPortSetting(),
	"AIRCRAFT_AIRLINERS": boolSetting("0"),
	"AIRCRAFT_MIN_ALT":   intSetting("0", 0, 10000),

	// Starlink trains
	"STARLINK_MAX_AGE_D": intSetting("10", 1, 30),

	// SPC convective outlook
	"SPC_DAY":      enumSetting("1", "1", "2", "3"),
	"SPC_MIN_RISK": enumSetting("2", "2", "3", "4", "5"),

	// Wildfires and smoke
	"FIRE_SMOKE":   boolSetting("1"),
	"FIRE_MIN_FRP": intSetting("0", 0, 500),

	// Satellite clouds
	"SATCLOUD_STYLE": enumSetting("white", "white", "enhanced"),

	// Rocket launches
	"LAUNCH_DAYS":   intSetting("1", 1, 30),
	"LAUNCH_LABELS": boolSetting("1"),

	// Planets, passes, tides, ships
	"PLANET_LABELS": boolSetting("1"),
	"PASS_SATS":     patternSetting("", noradListRe),
	"TIDE_STATION":  patternSetting("", tideIDRe),
	"TIDE_UNITS":    enumSetting("english", "english", "metric"),
	"SHIP_LABELS":   boolSetting("1"),

	// Aurora watch and light pollution
	"AURORA_MARGIN": intSetting("3", 0, 8),
	"AURORA_BANNER": boolSetting("1"),
	"LP_NOTE":       boolSetting("1"),

	// Global / local map view
	"VIEW_MODE":       enumSetting("global", "global", "local", "cycle"),
	"LOCAL_ZOOM":      enumSetting("8", "4", "6", "8", "12", "16", "20", "24"),
	"VIEW_GLOBAL_MIN": intSetting("5", 1, 240),
	"VIEW_LOCAL_MIN":  intSetting("5", 1, 240),
	"LOCAL_TOWNS":     boolSetting("1"),
	"HUD_UTC":         boolSetting("1"),
	"HUD_IP":          boolSetting("1"),
	"BASEMAP_SEASON":  boolSetting("1"),

	// Local layers
	"LSR_HOURS":       enumSetting("24", "6", "12", "24", "48"),
	"LSR_FOG":         boolSetting("0"),
	"RAIN_HOURS":      enumSetting("24", "1", "24", "48", "72"),
	"RIVER_RADIUS_KM": intSetting("150", 25, 500),
	"EBIRD_RADIUS_KM": intSetting("50", 5, 50),
	"EBIRD_DAYS":      intSetting("3", 1, 30),
	"COAST_BUOYS":     patternSetting("", buoyListRe),
	"COAST_METRIC":    boolSetting("0"),
	"PASS_MAP_HOURS":  intSetting("12", 1, 48),
	"DARK_SITES":      patternSetting("", darkSitesRe),

	// Home Assistant phone alerts
	"HA_NOTIFY_PASSES": boolSetting("1"),
	"HA_NOTIFY_AURORA": boolSetting("1"),
	"HA_PASS_LEAD_MIN": intSetting("10", 2, 60),
	"HA_PASS_MIN_EL":   intSetting("20", 10, 80),

	// Tonight's Sky applet (Astrospheric)
	"ASTRO_INTERVAL_H": intSetting("3", 1, 12),
	"ASTRO_CLEAR_PCT":  intSetting("20", 5, 60),
}

// GetRendererSettings returns every whitelisted setting with its current
// value (or default when unset).
func GetRendererSettings() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		result := make(map[string]string, len(rendererSettingDefs))
		for k, def := range rendererSettingDefs {
			result[k] = def.Default
		}
		data, _ := os.ReadFile(rendererConfPath)
		for _, line := range strings.Split(string(data), "\n") {
			k, v, ok := strings.Cut(strings.TrimSpace(line), "=")
			if !ok {
				continue
			}
			if _, known := rendererSettingDefs[k]; known {
				result[k] = strings.TrimSpace(v)
			}
		}
		jsonResponse(w, http.StatusOK, result)
	}
}

// PutRendererSettings validates and saves a subset of whitelisted
// settings, then signals the renderer to reload.
func PutRendererSettings(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req map[string]string
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}
		if len(req) == 0 {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "no settings"})
			return
		}
		for k, v := range req {
			req[k] = normalizeSetting(k, v)
		}
		for k, v := range req {
			def, ok := rendererSettingDefs[k]
			if !ok {
				jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "unknown setting " + k})
				return
			}
			if !def.Valid(v) {
				jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid value for " + k})
				return
			}
		}

		cfg.Lock()
		defer cfg.Unlock()

		data, _ := os.ReadFile(rendererConfPath)
		lines := strings.Split(strings.TrimRight(string(data), "\n"), "\n")
		if len(lines) == 1 && lines[0] == "" {
			lines = nil
		}
		written := make(map[string]bool, len(req))
		for i, line := range lines {
			k, _, ok := strings.Cut(strings.TrimSpace(line), "=")
			if !ok {
				continue
			}
			if v, set := req[k]; set {
				lines[i] = k + "=" + v
				written[k] = true
			}
		}
		for k, v := range req {
			if !written[k] {
				lines = append(lines, k+"="+v)
			}
		}

		if err := atomicWriteFile(rendererConfPath, []byte(strings.Join(lines, "\n")+"\n"), 0644); err != nil {
			internalError(w, "save renderer settings", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}
