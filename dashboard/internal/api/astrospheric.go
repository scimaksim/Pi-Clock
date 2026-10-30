package api

// Astrospheric API key — stored root-only on the persistent partition,
// separate from the world-readable renderer config. The key is write-only
// through the dashboard: GET reports whether one is set (and its last four
// characters), never the key itself.

import (
	"encoding/json"
	"net/http"
	"os"
	"regexp"
	"strings"
)

// astroKeyPath is a var so tests can redirect it.
var astroKeyPath = "/data/etc/pi-clock-astrospheric.key"

// Keys are opaque tokens; restrict to characters the renderer accepts so the
// value can be embedded in its JSON request without escaping.
var astroKeyRe = regexp.MustCompile(`^[A-Za-z0-9_-]{8,128}$`)

// GetAstrosphericKey reports whether a key is configured.
func GetAstrosphericKey() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := os.ReadFile(astroKeyPath)
		key := strings.TrimSpace(string(data))
		if err != nil || key == "" {
			jsonResponse(w, http.StatusOK, map[string]interface{}{"configured": false})
			return
		}
		hint := ""
		if len(key) >= 4 {
			hint = key[len(key)-4:]
		}
		jsonResponse(w, http.StatusOK, map[string]interface{}{
			"configured": true,
			"last4":      hint,
		})
	}
}

// PutAstrosphericKey saves (or, with an empty key, removes) the API key.
func PutAstrosphericKey() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Key string `json:"key"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}
		key := strings.TrimSpace(req.Key)
		if key == "" {
			if err := os.Remove(astroKeyPath); err != nil && !os.IsNotExist(err) {
				internalError(w, "remove astrospheric key", err)
				return
			}
			signalRenderer()
			jsonResponse(w, http.StatusOK, map[string]string{"status": "removed"})
			return
		}
		if !astroKeyRe.MatchString(key) {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "that doesn't look like an Astrospheric API key"})
			return
		}
		if err := atomicWriteFile(astroKeyPath, []byte(key+"\n"), 0600); err != nil {
			internalError(w, "save astrospheric key", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}
