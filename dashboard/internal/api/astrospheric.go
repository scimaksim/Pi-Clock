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

// secretKey describes one write-only API key file.
type secretKey struct {
	path  *string
	re    *regexp.Regexp
	label string
}

func (k secretKey) get() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := os.ReadFile(*k.path)
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

func (k secretKey) put() http.HandlerFunc {
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
			if err := os.Remove(*k.path); err != nil && !os.IsNotExist(err) {
				internalError(w, "remove "+k.label+" key", err)
				return
			}
			signalRenderer()
			jsonResponse(w, http.StatusOK, map[string]string{"status": "removed"})
			return
		}
		if !k.re.MatchString(key) {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "that doesn't look like an " + k.label + " API key"})
			return
		}
		if err := atomicWriteFile(*k.path, []byte(key+"\n"), 0600); err != nil {
			internalError(w, "save "+k.label+" key", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}

var astroKey = secretKey{&astroKeyPath, astroKeyRe, "Astrospheric"}

// GetAstrosphericKey reports whether a key is configured.
func GetAstrosphericKey() http.HandlerFunc { return astroKey.get() }

// PutAstrosphericKey saves (or, with an empty key, removes) the API key.
func PutAstrosphericKey() http.HandlerFunc { return astroKey.put() }

// aisstream.io key for the Ships layer — same write-only handling.
var aisKeyPath = "/data/etc/pi-clock-aisstream.key"

// aisstream keys are 40-hex-character tokens; allow the same safe set.
var aisKey = secretKey{&aisKeyPath, regexp.MustCompile(`^[A-Za-z0-9_-]{16,128}$`), "aisstream.io"}

// GetAISKey reports whether an aisstream.io key is configured.
func GetAISKey() http.HandlerFunc { return aisKey.get() }

// PutAISKey saves or removes the aisstream.io key.
func PutAISKey() http.HandlerFunc { return aisKey.put() }
