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
var hostPortRe = regexp.MustCompile(`^[A-Za-z0-9.\-]{0,253}(:[0-9]{1,5})?$`)

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
