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

func importBackup(t *testing.T, b settingsBackup) *httptest.ResponseRecorder {
	t.Helper()
	body, _ := json.Marshal(b)
	rec := httptest.NewRecorder()
	ImportSettings(nil)(rec, httptest.NewRequest(http.MethodPost, "/", strings.NewReader(string(body))))
	return rec
}

func restorePaths(t *testing.T) string {
	dir := t.TempDir()
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	backupLayersPath = filepath.Join(dir, "layers.conf")
	backupAppletsPath = filepath.Join(dir, "applets.conf")
	backupSatellitesPath = filepath.Join(dir, "sats.conf")
	backupTickerPath = filepath.Join(dir, "ticker.conf")
	astroKeyPath = filepath.Join(dir, "astro.key")
	os.WriteFile(rendererConfPath, []byte("QTH_LAT=35.69\n"), 0644)
	os.WriteFile(backupLayersPath, []byte("radar=1,0.80\n"), 0644)
	return dir
}

// A destination that can't be written must leave every file as it was.
func TestImportIsAllOrNothing(t *testing.T) {
	dir := restorePaths(t)
	backupAppletsPath = filepath.Join(dir, "missing-dir", "applets.conf")
	rec := importBackup(t, settingsBackup{
		Format: backupFormat, Version: backupVersion,
		Renderer: map[string]string{"QTH_LAT": "40.00"},
		Lists:    map[string][]string{"layers": {"radar=0,0.50"}, "applets": {"clock=1,left"}},
	})
	if rec.Code == http.StatusOK {
		t.Fatalf("import reported success: %s", rec.Body)
	}
	if got, _ := os.ReadFile(rendererConfPath); string(got) != "QTH_LAT=35.69\n" {
		t.Fatalf("renderer settings changed by a failed import:\n%s", got)
	}
	if got, _ := os.ReadFile(backupLayersPath); string(got) != "radar=1,0.80\n" {
		t.Fatalf("layers changed by a failed import:\n%s", got)
	}
	left, _ := os.ReadDir(dir)
	for _, e := range left {
		if strings.Contains(e.Name(), ".tmp") {
			t.Fatalf("temporary file left behind: %s", e.Name())
		}
	}
}

// An existing renderer file that can't be read is an error, not "empty":
// treating it as empty would replace every setting it holds.
func TestImportRefusesUnreadableSettings(t *testing.T) {
	restorePaths(t)
	os.Remove(rendererConfPath)
	os.Mkdir(rendererConfPath, 0755) // reading a directory fails
	rec := importBackup(t, settingsBackup{
		Format: backupFormat, Version: backupVersion,
		Renderer: map[string]string{"QTH_LAT": "40.00"},
		Lists:    map[string][]string{"layers": {"radar=0,0.50"}},
	})
	if rec.Code == http.StatusOK {
		t.Fatalf("import reported success: %s", rec.Body)
	}
	if got, _ := os.ReadFile(backupLayersPath); string(got) != "radar=1,0.80\n" {
		t.Fatalf("layers changed although the import failed:\n%s", got)
	}
}

// A rename that fails part-way puts back the files already replaced.
func TestImportRollsBackAFailedRename(t *testing.T) {
	restorePaths(t)
	calls := 0
	renameFile = func(from, to string) error {
		calls++
		if calls == 2 {
			return os.ErrPermission
		}
		return os.Rename(from, to)
	}
	defer func() { renameFile = os.Rename }()
	rec := importBackup(t, settingsBackup{
		Format: backupFormat, Version: backupVersion,
		Renderer: map[string]string{"QTH_LAT": "40.00"},
		Lists:    map[string][]string{"layers": {"radar=0,0.50"}, "applets": {"clock=1,left"}},
	})
	if rec.Code == http.StatusOK {
		t.Fatalf("import reported success: %s", rec.Body)
	}
	if got, _ := os.ReadFile(rendererConfPath); string(got) != "QTH_LAT=35.69\n" {
		t.Fatalf("renderer settings not restored:\n%s", got)
	}
	if got, _ := os.ReadFile(backupLayersPath); string(got) != "radar=1,0.80\n" {
		t.Fatalf("layers changed:\n%s", got)
	}
	if _, err := os.Stat(backupAppletsPath); !os.IsNotExist(err) {
		t.Fatal("applets file created by a failed import")
	}
}
