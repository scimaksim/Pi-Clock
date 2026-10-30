package api

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/config"
)

func TestRendererSettingsRoundTrip(t *testing.T) {
	dir := t.TempDir()
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	if err := os.WriteFile(rendererConfPath,
		[]byte("EARTHQUAKE_DISPLAY_S=3600\nVOLCANO_LABELS=1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	cfg := &config.RendererConfig{}

	put := func(body string) int {
		req := httptest.NewRequest(http.MethodPut, "/api/renderer-settings", strings.NewReader(body))
		rec := httptest.NewRecorder()
		PutRendererSettings(cfg)(rec, req)
		return rec.Code
	}

	if code := put(`{"VOLCANO_LABELS":"0","VOLCANO_NEW_ONLY":"1"}`); code != http.StatusOK {
		t.Fatalf("valid PUT: got %d", code)
	}
	if code := put(`{"VOLCANO_LABELS":"yes"}`); code != http.StatusBadRequest {
		t.Fatalf("invalid value: got %d", code)
	}
	if code := put(`{"EARTHQUAKE_DISPLAY_S":"1"}`); code != http.StatusBadRequest {
		t.Fatalf("non-whitelisted key: got %d", code)
	}
	if code := put(`{"VOLCANO_LABELS":"1\nEVIL=1"}`); code != http.StatusBadRequest {
		t.Fatalf("newline injection: got %d", code)
	}

	data, _ := os.ReadFile(rendererConfPath)
	got := string(data)
	for _, want := range []string{"EARTHQUAKE_DISPLAY_S=3600\n", "VOLCANO_LABELS=0\n", "VOLCANO_NEW_ONLY=1\n"} {
		if !strings.Contains(got, want) {
			t.Errorf("config missing %q:\n%s", want, got)
		}
	}
	if strings.Count(got, "VOLCANO_LABELS=") != 1 {
		t.Errorf("duplicate key written:\n%s", got)
	}

	rec := httptest.NewRecorder()
	GetRendererSettings()(rec, httptest.NewRequest(http.MethodGet, "/api/renderer-settings", nil))
	if !strings.Contains(rec.Body.String(), `"VOLCANO_NEW_ONLY":"1"`) {
		t.Errorf("GET body: %s", rec.Body.String())
	}
	if strings.Contains(rec.Body.String(), "EARTHQUAKE_DISPLAY_S") {
		t.Errorf("GET leaked non-whitelisted key: %s", rec.Body.String())
	}
}
