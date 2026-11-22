/*
 * backup.go - Export / import of dashboard settings
 *
 * GET  /api/settings/export[?keys=1]  -> JSON file with every setting
 * POST /api/settings/import           <- the same JSON
 *
 * The export covers the renderer settings, layer, panel, satellite and
 * ticker lists. API keys and the Home Assistant webhook are included
 * only when keys=1 is asked for. System files (password, WiFi,
 * hostname, TLS certs) are never exported.
 *
 * Import validates everything before writing anything: renderer keys
 * known to rendererSettingDefs go through their own validator, other
 * keys must be plain KEY=value with a conservative charset, list files
 * must be name=value lines, and keys must pass their own patterns.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/config"
)

const backupFormat = "pi-clock-settings"
const backupVersion = 1

// backupListFile is a name=value config file exported as an ordered
// list of lines (order matters for applets).
type backupListFile struct {
	id   string
	path *string
}

var (
	backupLayersPath     = layersConfPath
	backupAppletsPath    = appletsConfPath
	backupSatellitesPath = satConfPath
	backupTickerPath     = tickerConfPath
)

var backupLists = []backupListFile{
	{"layers", &backupLayersPath},
	{"applets", &backupAppletsPath},
	{"satellites", &backupSatellitesPath},
	{"ticker", &backupTickerPath},
}

var backupSecrets = map[string]secretKey{
	"astrospheric": astroKey,
	"aisstream":    aisKey,
	"ebird":        ebirdKey,
	"ha_webhook":   haWebhook,
}

// Renderer keys that only describe this particular Pi and must not be
// copied to another one.
var backupSkipRenderer = map[string]bool{
	"CPU_CORES": true, "RAM_TOTAL": true, "RAM_BUDGET": true,
	"ACTUAL_REFRESH": true, "COMPLETED": true, "RECONFIGURE_FAILED": true,
}

var (
	backupKeyRe      = regexp.MustCompile(`^[A-Z][A-Z0-9_]{0,47}$`)
	backupValueRe    = regexp.MustCompile(`^[A-Za-z0-9._,:/+-]{0,128}$`)
	backupListLineRe = regexp.MustCompile(`^[a-z][a-z0-9_]{0,47}=[A-Za-z0-9.,:_-]{0,64}$`)
)

type settingsBackup struct {
	Format   string              `json:"format"`
	Version  int                 `json:"version"`
	Exported string              `json:"exported,omitempty"`
	Host     string              `json:"host,omitempty"`
	Renderer map[string]string   `json:"renderer"`
	Lists    map[string][]string `json:"lists"`
	Secrets  map[string]string   `json:"secrets,omitempty"`
	skipped  []string            // invalid values dropped on import
}

func readKV(path string) (map[string]string, []string) {
	kv, order, _ := readKVChecked(path)
	return kv, order
}

// readKVChecked is readKV that reports a file it could not read; a file
// that does not exist yet reads as empty.
func readKVChecked(path string) (map[string]string, []string, error) {
	kv := map[string]string{}
	var order []string
	data, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return nil, nil, err
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		if _, seen := kv[k]; !seen {
			order = append(order, k)
		}
		kv[k] = v
	}
	return kv, order, nil
}

func readListLines(path string) []string {
	var out []string
	data, _ := os.ReadFile(path)
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		out = append(out, line)
	}
	return out
}

// ExportSettings returns all settings as a downloadable JSON file.
func ExportSettings() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		b := settingsBackup{
			Format:   backupFormat,
			Version:  backupVersion,
			Exported: time.Now().UTC().Format(time.RFC3339),
			Renderer: map[string]string{},
			Lists:    map[string][]string{},
		}
		if h, err := os.Hostname(); err == nil {
			b.Host = h
		}
		kv, _ := readKV(rendererConfPath)
		for k, v := range kv {
			if !backupSkipRenderer[k] {
				b.Renderer[k] = v
			}
		}
		for _, f := range backupLists {
			if lines := readListLines(*f.path); lines != nil {
				b.Lists[f.id] = lines
			}
		}
		if r.URL.Query().Get("keys") == "1" {
			b.Secrets = map[string]string{}
			for id, s := range backupSecrets {
				data, err := os.ReadFile(*s.path)
				if v := strings.TrimSpace(string(data)); err == nil && v != "" {
					b.Secrets[id] = v
				}
			}
		}
		name := "pi-clock-settings"
		if b.Host != "" && regexp.MustCompile(`^[A-Za-z0-9-]{1,63}$`).MatchString(b.Host) {
			name += "-" + b.Host
		}
		name += "-" + time.Now().Format("2006-01-02") + ".json"
		w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
		w.Header().Set("Cache-Control", "no-store")
		jsonResponse(w, http.StatusOK, b)
	}
}

// Core station/display keys are written by their own dashboard
// handlers rather than rendererSettingDefs; check them the same way
// before importing.
func floatIn(min, max float64) func(string) bool {
	return func(v string) bool {
		f, err := strconv.ParseFloat(v, 64)
		return err == nil && f >= min && f <= max
	}
}

var backupCoreValid = map[string]func(string) bool{
	"QTH_LAT":            floatIn(-90, 90),
	"QTH_LON":            floatIn(-180, 180),
	"CENTER_LON":         floatIn(-180, 180),
	"MAP_ZOOM":           intSetting("0", 0, 100).Valid,
	"DISPLAY_RESOLUTION": enumSetting("1080p", "native", "720p", "1080p", "1440p", "4k").Valid,
	"DISPLAY_ROTATE":     enumSetting("0", "0", "180").Valid,
	"CALLSIGN":           regexp.MustCompile(`^[A-Za-z0-9/]{0,16}$`).MatchString,
	"GRID_SQUARE":        regexp.MustCompile(`^([A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?)?$`).MatchString,
	"DX_BANDS":           regexp.MustCompile(`^[0-9A-Fa-f]{1,8}$`).MatchString,
	"DX_DISTANCE":        intSetting("0", 0, 25000).Valid,
	"DX_SPOT_AGE":        intSetting("900", 0, 86400).Valid,
}

// validateBackup checks the whole file before anything is written.
// Structural problems (wrong format, unknown section, bad list line,
// bad key) reject the file. A renderer setting whose value doesn't
// pass its check is dropped with a warning instead, so one
// hand-edited value doesn't block the rest.
func validateBackup(b *settingsBackup) error {
	if b.Format != backupFormat {
		return fmt.Errorf("not a Pi-Clock settings file")
	}
	if b.Version < 1 || b.Version > backupVersion {
		return fmt.Errorf("settings file version %d is newer than this dashboard", b.Version)
	}
	if len(b.Renderer) > 400 {
		return fmt.Errorf("too many renderer settings")
	}
	for k, v := range b.Renderer {
		if !backupKeyRe.MatchString(k) {
			return fmt.Errorf("invalid renderer setting %q", k)
		}
		v = normalizeSetting(k, v)
		ok := false
		if def, known := rendererSettingDefs[k]; known {
			ok = def.Valid(v)
		} else if check, core := backupCoreValid[k]; core {
			ok = check(v)
		} else {
			ok = backupValueRe.MatchString(v)
		}
		if !ok {
			delete(b.Renderer, k)
			b.skipped = append(b.skipped, k)
			continue
		}
		b.Renderer[k] = v
	}
	known := map[string]bool{}
	for _, f := range backupLists {
		known[f.id] = true
	}
	for id, lines := range b.Lists {
		if !known[id] {
			return fmt.Errorf("unknown section %q", id)
		}
		if len(lines) > 300 {
			return fmt.Errorf("too many entries in %s", id)
		}
		for _, l := range lines {
			if !backupListLineRe.MatchString(l) {
				return fmt.Errorf("invalid line in %s", id)
			}
		}
	}
	for id, v := range b.Secrets {
		s, ok := backupSecrets[id]
		if !ok {
			return fmt.Errorf("unknown key %q", id)
		}
		if !s.re.MatchString(v) {
			return fmt.Errorf("the %s key in the file isn't valid", s.label)
		}
	}
	return nil
}

// ImportSettings applies a settings file produced by ExportSettings.
// Renderer keys in the file replace the current values; keys that are
// not in the file are left as they are. The list files are replaced.
func ImportSettings(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		r.Body = http.MaxBytesReader(w, r.Body, 256<<10)
		var b settingsBackup
		if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "not a valid settings file"})
			return
		}
		if err := validateBackup(&b); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}

		if cfg != nil {
			cfg.Lock()
		}
		kv, order, err := readKVChecked(rendererConfPath)
		if err != nil {
			if cfg != nil {
				cfg.Unlock()
			}
			internalError(w, "read renderer settings", err)
			return
		}
		var keys []string
		for k := range b.Renderer {
			if backupSkipRenderer[k] {
				continue
			}
			if _, ok := kv[k]; !ok {
				keys = append(keys, k)
			}
			kv[k] = b.Renderer[k]
		}
		sort.Strings(keys)
		order = append(order, keys...)
		var out strings.Builder
		out.WriteString("# Pi-Clock renderer configuration\n")
		out.WriteString("# Imported from settings file " + time.Now().Format("2006-01-02 15:04") + "\n")
		for _, k := range order {
			out.WriteString(k + "=" + kv[k] + "\n")
		}

		// Every file is written to a temp file first; only when all of
		// them are ready are they moved into place, so a destination that
		// can't be written leaves every setting as it was.
		changes := []fileChange{{rendererConfPath, []byte(out.String()), 0644}}
		for _, f := range backupLists {
			lines, ok := b.Lists[f.id]
			if !ok {
				continue
			}
			content := "# Pi-Clock " + f.id + " configuration (imported)\n" + strings.Join(lines, "\n") + "\n"
			changes = append(changes, fileChange{*f.path, []byte(content), 0644})
		}
		nkeys := 0
		for id, v := range b.Secrets {
			changes = append(changes, fileChange{*backupSecrets[id].path, []byte(v + "\n"), 0600})
			nkeys++
		}
		err = replaceAll(changes)
		if cfg != nil {
			cfg.Unlock()
		}
		if err != nil {
			internalError(w, "import settings", err)
			return
		}
		if cfg != nil {
			cfg.Reload()
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]interface{}{
			"status":   "imported",
			"settings": len(b.Renderer),
			"keys":     nkeys,
			"skipped":  b.skipped,
		})
	}
}

// renameFile is os.Rename; the tests replace it to fail one rename.
var renameFile = os.Rename

// fileChange is one file an import replaces.
type fileChange struct {
	path string
	data []byte
	perm os.FileMode
}

// replaceAll replaces every file or none. All new contents are written to
// temp files first; any failure there removes them and changes nothing.
// Then they are renamed into place; should a rename fail, the files
// already replaced are put back from their earlier contents (or removed
// if they did not exist), and the error says so if that also fails.
func replaceAll(changes []fileChange) error {
	tmps := make([]string, 0, len(changes))
	cleanup := func() {
		for _, t := range tmps {
			os.Remove(t)
		}
	}
	type prior struct {
		data    []byte
		existed bool
		perm    os.FileMode
	}
	priors := make([]prior, len(changes))
	for i, c := range changes {
		old, err := os.ReadFile(c.path)
		switch {
		case err == nil:
			perm := c.perm
			if st, serr := os.Stat(c.path); serr == nil {
				perm = st.Mode().Perm()
			}
			priors[i] = prior{old, true, perm}
		case os.IsNotExist(err):
		default:
			cleanup()
			return fmt.Errorf("read %s: %w", c.path, err)
		}
		tmp, err := writeTemp(c.path, c.data, c.perm)
		if err != nil {
			cleanup()
			return err
		}
		tmps = append(tmps, tmp)
	}
	for i, c := range changes {
		if err := renameFile(tmps[i], c.path); err != nil {
			tmps = tmps[i:]
			cleanup()
			var undo []string
			for j := 0; j < i; j++ {
				p := priors[j]
				var uerr error
				if p.existed {
					uerr = atomicWriteFile(changes[j].path, p.data, p.perm)
				} else {
					uerr = os.Remove(changes[j].path)
				}
				if uerr != nil {
					undo = append(undo, changes[j].path)
				}
			}
			if len(undo) > 0 {
				return fmt.Errorf("replace %s: %w; could not restore %s", c.path, err, strings.Join(undo, ", "))
			}
			return fmt.Errorf("replace %s: %w (earlier files restored)", c.path, err)
		}
	}
	return nil
}
