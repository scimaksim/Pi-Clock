package api

// Astrospheric API key — stored root-only on the persistent partition,
// separate from the world-readable renderer config. The key is write-only
// through the dashboard: GET reports whether one is set (and its last four
// characters), never the key itself.

import (
	"bytes"
	"encoding/json"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"
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

// Home Assistant webhook for phone alerts (passes, aurora). The URL's
// last path segment is the webhook ID, which acts as a shared secret,
// so it is stored root-only and never returned.
var haWebhookPath = "/data/etc/pi-clock-ha-webhook"

var haWebhookRe = regexp.MustCompile(`^https?://[A-Za-z0-9.\-]{1,253}(:[0-9]{1,5})?/api/webhook/[A-Za-z0-9_\-]{6,128}$`)

var haWebhook = secretKey{&haWebhookPath, haWebhookRe, "Home Assistant webhook"}

// GetHAWebhook reports whether a webhook URL is configured.
func GetHAWebhook() http.HandlerFunc { return haWebhook.get() }

// PutHAWebhook saves or removes the webhook URL.
func PutHAWebhook() http.HandlerFunc { return haWebhook.put() }

// TestHAWebhook posts a sample alert so the user can check their
// automation end to end.
func TestHAWebhook() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := os.ReadFile(haWebhookPath)
		url := strings.TrimSpace(string(data))
		if err != nil || !haWebhookRe.MatchString(url) {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "no webhook URL saved"})
			return
		}
		body, _ := json.Marshal(map[string]interface{}{
			"source":  "pi-clock",
			"event":   "test",
			"title":   "Pi-Clock test",
			"message": "Alerts from Pi-Clock are working.",
		})
		client := &http.Client{Timeout: 10 * time.Second}
		resp, err := client.Post(url, "application/json", bytes.NewReader(body))
		if err != nil {
			jsonResponse(w, http.StatusBadGateway, map[string]string{"error": "couldn't reach Home Assistant"})
			return
		}
		resp.Body.Close()
		if resp.StatusCode >= 300 {
			jsonResponse(w, http.StatusBadGateway, map[string]string{"error": "Home Assistant answered " + resp.Status})
			return
		}
		jsonResponse(w, http.StatusOK, map[string]string{"status": "sent"})
	}
}

// eBird API key for the Rare Birds layer.
var ebirdKeyPath = "/data/etc/pi-clock-ebird.key"

var ebirdKey = secretKey{&ebirdKeyPath, regexp.MustCompile(`^[A-Za-z0-9]{6,64}$`), "eBird"}

// GetEBirdKey reports whether an eBird key is configured.
func GetEBirdKey() http.HandlerFunc { return ebirdKey.get() }

// PutEBirdKey saves or removes the eBird key.
func PutEBirdKey() http.HandlerFunc { return ebirdKey.put() }

// Space-Track.org account (back-up source of satellite orbits when
// CelesTrak can't be reached). Stored root-only as "identity\npassword\n";
// never read back: GET says only whether one is saved, with a masked
// username.
var spaceTrackPath = "/data/etc/pi-clock-spacetrack.cred"

var stIdentityRe = regexp.MustCompile(`^[^\s\x00-\x1f]{3,128}$`)

func maskIdentity(id string) string {
	if at := strings.Index(id, "@"); at > 0 {
		return id[:1] + "…" + id[at:]
	}
	if len(id) > 2 {
		return id[:2] + "…"
	}
	return "…"
}

// GetSpaceTrack reports whether a Space-Track account is saved.
func GetSpaceTrack() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		data, err := os.ReadFile(spaceTrackPath)
		lines := strings.SplitN(string(data), "\n", 3)
		if err != nil || len(lines) < 2 || strings.TrimSpace(lines[0]) == "" {
			jsonResponse(w, http.StatusOK, map[string]interface{}{"configured": false})
			return
		}
		jsonResponse(w, http.StatusOK, map[string]interface{}{
			"configured": true,
			"user":       maskIdentity(strings.TrimSpace(lines[0])),
		})
	}
}

// PutSpaceTrack saves the account, or removes it when both are empty.
func PutSpaceTrack() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Identity string `json:"identity"`
			Password string `json:"password"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}
		id := strings.TrimSpace(req.Identity)
		if id == "" && req.Password == "" {
			if err := os.Remove(spaceTrackPath); err != nil && !os.IsNotExist(err) {
				internalError(w, "remove Space-Track account", err)
				return
			}
			signalRenderer()
			jsonResponse(w, http.StatusOK, map[string]string{"status": "removed"})
			return
		}
		if !stIdentityRe.MatchString(id) {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "enter your Space-Track username (email)"})
			return
		}
		if len(req.Password) < 1 || len(req.Password) > 128 || strings.ContainsAny(req.Password, "\r\n\x00") {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "enter your Space-Track password"})
			return
		}
		if err := atomicWriteFile(spaceTrackPath, []byte(id+"\n"+req.Password+"\n"), 0600); err != nil {
			internalError(w, "save Space-Track account", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}
