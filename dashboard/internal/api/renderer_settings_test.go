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

func TestDarkSitesNormalized(t *testing.T) {
	cases := map[string]string{
		"Jordan Lake:35.70:-79.07; Falls Lake:36.01:-78.69": "Jordan_Lake:35.70:-79.07,Falls_Lake:36.01:-78.69",
		"Bob's (Field):1:2": "Bobs_Field:1:2",
		"":                  "",
	}
	for in, want := range cases {
		got := normalizeSetting("DARK_SITES", in)
		if got != want {
			t.Fatalf("%q -> %q want %q", in, got, want)
		}
		if got != "" && !darkSitesRe.MatchString(got) {
			t.Fatalf("%q not valid after normalising", got)
		}
	}
	for _, bad := range []string{"A:1:2;touch pwned :1:2", "x$(id):1:2", "a:1:2|b:3:4"} {
		v := normalizeSetting("DARK_SITES", bad)
		if darkSitesRe.MatchString(v) && strings.ContainsAny(v, " ;$()|'\"`") {
			t.Fatalf("unsafe value accepted: %q", v)
		}
	}
}

func TestWorldSettings(t *testing.T) {
	ok := map[string][]string{
		"VIEW_WORLDS":    {"", "moon", "mars,jupiter", "moon,mars,jupiter"},
		"VIEW_MODE":      {"cycle", "moon", "mars", "jupiter"},
		"JUPITER_GRS_L2": {"-1", "0", "100", "359"},
		"VIEW_WORLD_MIN": {"1", "2", "30"},
	}
	bad := map[string][]string{
		"VIEW_WORLDS":    {"pluto", "moon,", "moon;reboot", "moon,mars,jupiter,moon", "MOON"},
		"VIEW_MODE":      {"venus"},
		"JUPITER_GRS_L2": {"360", "-2", "1.5", ""},
		"VIEW_WORLD_MIN": {"0", "31"},
	}
	for k, vs := range ok {
		for _, v := range vs {
			if !rendererSettingDefs[k].Valid(v) {
				t.Fatalf("%s=%q rejected", k, v)
			}
		}
	}
	for k, vs := range bad {
		for _, v := range vs {
			if rendererSettingDefs[k].Valid(v) {
				t.Fatalf("%s=%q accepted", k, v)
			}
		}
	}
}
