package api

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestAstrosphericKey(t *testing.T) {
	astroKeyPath = filepath.Join(t.TempDir(), "astro.key")

	put := func(body string) int {
		rec := httptest.NewRecorder()
		PutAstrosphericKey()(rec, httptest.NewRequest(http.MethodPut, "/", strings.NewReader(body)))
		return rec.Code
	}
	get := func() string {
		rec := httptest.NewRecorder()
		GetAstrosphericKey()(rec, httptest.NewRequest(http.MethodGet, "/", nil))
		return rec.Body.String()
	}

	if !strings.Contains(get(), `"configured":false`) {
		t.Fatal("expected no key")
	}
	if put(`{"key":"bad key\"}"}`) != http.StatusBadRequest {
		t.Fatal("accepted invalid key")
	}
	if put(`{"key":"abcDEF123-xyz_9876"}`) != http.StatusOK {
		t.Fatal("rejected valid key")
	}
	st, _ := os.Stat(astroKeyPath)
	if st.Mode().Perm() != 0600 {
		t.Fatalf("key file mode %v, want 0600", st.Mode().Perm())
	}
	body := get()
	if !strings.Contains(body, `"last4":"9876"`) || strings.Contains(body, "abcDEF") {
		t.Fatalf("GET leaked or missing hint: %s", body)
	}
	if put(`{"key":""}`) != http.StatusOK {
		t.Fatal("remove failed")
	}
	if _, err := os.Stat(astroKeyPath); !os.IsNotExist(err) {
		t.Fatal("key file not removed")
	}
}

func TestAISKeyAndPatternSettings(t *testing.T) {
	aisKeyPath = filepath.Join(t.TempDir(), "ais.key")
	rec := httptest.NewRecorder()
	PutAISKey()(rec, httptest.NewRequest(http.MethodPut, "/", strings.NewReader(`{"key":"0123456789abcdef0123456789abcdef01234567"}`)))
	if rec.Code != http.StatusOK {
		t.Fatalf("save: %d %s", rec.Code, rec.Body)
	}
	if st, _ := os.Stat(aisKeyPath); st == nil || st.Mode().Perm() != 0600 {
		t.Fatalf("key file must be 0600")
	}
	rec = httptest.NewRecorder()
	PutAISKey()(rec, httptest.NewRequest(http.MethodPut, "/", strings.NewReader(`{"key":"bad key\nINJECT=1"}`)))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("bad key accepted: %d", rec.Code)
	}
	for v, ok := range map[string]bool{"": true, "25544": true, "25544,48274,20580": true,
		"25544,": false, "1,2,3,4,5,6,7": false, "abc": false} {
		if rendererSettingDefs["PASS_SATS"].Valid(v) != ok {
			t.Errorf("PASS_SATS %q: want %v", v, ok)
		}
	}
	for v, ok := range map[string]bool{"": true, "8658163": true, "TEC2863": true, "86 58": false, "x\nY=1": false} {
		if rendererSettingDefs["TIDE_STATION"].Valid(v) != ok {
			t.Errorf("TIDE_STATION %q: want %v", v, ok)
		}
	}
}

func TestHAWebhookValidation(t *testing.T) {
	for v, ok := range map[string]bool{
		"http://192.168.1.90:8123/api/webhook/pi-clock-abc123": true,
		"https://ha.example.com/api/webhook/Xy_9-abcdef":       true,
		"http://192.168.1.90:8123/api/services/x":              false,
		"http://192.168.1.90:8123/api/webhook/abc def":         false,
		"file:///etc/passwd":                                   false,
		"http://h/api/webhook/abcdef\nX=1":                     false,
	} {
		if haWebhookRe.MatchString(v) != ok {
			t.Errorf("%q: want %v", v, ok)
		}
	}
}

func TestLocalSettingsValidation(t *testing.T) {
	for v, ok := range map[string]bool{
		"": true, "Jordan Lake:35.736:-79.017": true,
		"Backyard:35.69:-78.71;Medoc Mountain SP:36.264:-77.888": true,
		"Bad\nName:1:2": false, "X:1:2;": true, "X=1:2:3": false,
	} {
		if rendererSettingDefs["DARK_SITES"].Valid(normalizeSetting("DARK_SITES", v)) != ok {
			t.Errorf("DARK_SITES %q: want %v", v, ok)
		}
	}
	for v, ok := range map[string]bool{"": true, "41110": true, "41110,41025,41013": true, "4111 0": false} {
		if rendererSettingDefs["COAST_BUOYS"].Valid(v) != ok {
			t.Errorf("COAST_BUOYS %q: want %v", v, ok)
		}
	}
}
