package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSettingsExportImport(t *testing.T) {
	dir := t.TempDir()
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	backupLayersPath = filepath.Join(dir, "layers.conf")
	backupAppletsPath = filepath.Join(dir, "applets.conf")
	backupSatellitesPath = filepath.Join(dir, "sats.conf")
	backupTickerPath = filepath.Join(dir, "ticker.conf")
	astroKeyPath = filepath.Join(dir, "astro.key")
	aisKeyPath = filepath.Join(dir, "ais.key")
	ebirdKeyPath = filepath.Join(dir, "ebird.key")
	haWebhookPath = filepath.Join(dir, "ha")

	os.WriteFile(rendererConfPath, []byte("# c\nQTH_LAT=35.69\nCPU_CORES=4\nVIEW_MODE=local\nDX_BANDS=0FFF\n"), 0644)
	os.WriteFile(backupLayersPath, []byte("# l\nradar=1,0.80\nsatclouds=0,0.74\n"), 0644)
	os.WriteFile(backupAppletsPath, []byte("clock=1,left\npasses=1,right\n"), 0644)
	os.WriteFile(astroKeyPath, []byte("abcDEF123-xyz_9876\n"), 0600)

	export := func(q string) settingsBackup {
		rec := httptest.NewRecorder()
		ExportSettings()(rec, httptest.NewRequest(http.MethodGet, "/"+q, nil))
		var b settingsBackup
		if err := json.Unmarshal(rec.Body.Bytes(), &b); err != nil {
			t.Fatal(err)
		}
		return b
	}
	b := export("")
	if b.Secrets != nil || b.Renderer["QTH_LAT"] != "35.69" || b.Renderer["CPU_CORES"] != "" {
		t.Fatalf("bad export %+v", b)
	}
	if len(b.Lists["applets"]) != 2 || b.Lists["applets"][1] != "passes=1,right" {
		t.Fatalf("applets %v", b.Lists["applets"])
	}
	bk := export("?keys=1")
	if bk.Secrets["astrospheric"] != "abcDEF123-xyz_9876" {
		t.Fatal("key not exported")
	}

	// Wipe and import into a "fresh" Pi.
	os.WriteFile(rendererConfPath, []byte("CPU_CORES=2\nQTH_LAT=0\n"), 0644)
	os.Remove(backupLayersPath)
	os.Remove(astroKeyPath)
	imp := func(v interface{}) int {
		body, _ := json.Marshal(v)
		rec := httptest.NewRecorder()
		ImportSettings(nil)(rec, httptest.NewRequest(http.MethodPost, "/", strings.NewReader(string(body))))
		return rec.Code
	}
	if c := imp(bk); c != http.StatusOK {
		t.Fatalf("import %d", c)
	}
	got, _ := os.ReadFile(rendererConfPath)
	if !strings.Contains(string(got), "QTH_LAT=35.69\n") || !strings.Contains(string(got), "CPU_CORES=2\n") ||
		!strings.Contains(string(got), "VIEW_MODE=local\n") {
		t.Fatalf("renderer after import:\n%s", got)
	}
	if l, _ := os.ReadFile(backupLayersPath); !strings.Contains(string(l), "satclouds=0,0.74") {
		t.Fatal("layers not restored")
	}
	if st, err := os.Stat(astroKeyPath); err != nil || st.Mode().Perm() != 0600 {
		t.Fatal("key not restored 0600")
	}

	// Structurally hostile files are rejected without writing anything.
	before, _ := os.ReadFile(rendererConfPath)
	bad := []settingsBackup{
		{Format: "x", Version: 1},
		{Format: backupFormat, Version: 99},
		{Format: backupFormat, Version: 1, Renderer: map[string]string{"foo\nX": "1"}},
		{Format: backupFormat, Version: 1, Lists: map[string][]string{"layers": {"radar=1\nX=2"}}},
		{Format: backupFormat, Version: 1, Lists: map[string][]string{"../etc": {"a=1"}}},
		{Format: backupFormat, Version: 1, Secrets: map[string]string{"astrospheric": "bad key"}},
		{Format: backupFormat, Version: 1, Secrets: map[string]string{"shadow": "abcdefgh"}},
	}
	for i, x := range bad {
		if c := imp(x); c != http.StatusBadRequest {
			t.Fatalf("bad[%d] accepted (%d)", i, c)
		}
	}
	if after, _ := os.ReadFile(rendererConfPath); string(after) != string(before) {
		t.Fatal("rejected import modified renderer.conf")
	}

	// Bad values are dropped (never written); the rest imports.
	mixed := settingsBackup{Format: backupFormat, Version: 1, Renderer: map[string]string{
		"FOO": "a;reboot", "BAR": "$(id)", "VIEW_MODE": "bogus", "QTH_LAT": "999",
		"DISPLAY_RESOLUTION": "8k", "LOCAL_ZOOM": "12", "DARK_SITES": "Jordan Lake:35.7:-79.0",
	}}
	if c := imp(mixed); c != http.StatusOK {
		t.Fatalf("mixed import %d", c)
	}
	got, _ = os.ReadFile(rendererConfPath)
	for _, gone := range []string{"FOO=", "BAR=", "VIEW_MODE=bogus", "QTH_LAT=999", "DISPLAY_RESOLUTION=8k", "reboot", "$("} {
		if strings.Contains(string(got), gone) {
			t.Fatalf("invalid value %q written:\n%s", gone, got)
		}
	}
	if !strings.Contains(string(got), "LOCAL_ZOOM=12\n") || !strings.Contains(string(got), "DARK_SITES=Jordan_Lake:35.7:-79.0\n") {
		t.Fatalf("valid values missing:\n%s", got)
	}
}
