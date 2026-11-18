package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestGetDataStatus(t *testing.T) {
	dir := t.TempDir()
	oldS, oldC := statusPath, rendererConfPath
	statusPath = filepath.Join(dir, "status.json")
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	defer func() { statusPath, rendererConfPath = oldS, oldC }()

	// No file yet
	rec := httptest.NewRecorder()
	GetDataStatus()(rec, httptest.NewRequest(http.MethodGet, "/api/data-status", nil))
	var out map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &out); err != nil || out["available"] != false {
		t.Fatalf("missing file: %v %s", err, rec.Body.String())
	}

	os.WriteFile(rendererConfPath, []byte("PC_HELPER_URL=http://192.168.1.159:8765\n"), 0644)
	os.WriteFile(statusPath, []byte(`{"updated":100,"sources":[
	 {"host":"ll.thespacedevs.com","users":["launches"],"last_ok":90,"streak":0,"ok":3,"fail":0,"error":""},
	 {"host":"celestrak.org","users":["starlink"],"last_ok":0,"last_fail":95,"fail_since":10,"streak":9,"ok":0,"fail":9,"error":"timed out"},
	 {"host":"192.168.1.159","users":["pch_birds"],"last_ok":99,"streak":1,"ok":5,"fail":1,"error":"x"},
	 {"host":"192.168.1.90","users":["hanotify"],"last_ok":50,"streak":0,"ok":1,"fail":0,"error":""}]}`), 0644)
	rec = httptest.NewRecorder()
	GetDataStatus()(rec, httptest.NewRequest(http.MethodGet, "/api/data-status", nil))
	var res struct {
		Available bool           `json:"available"`
		Sources   []sourceStatus `json:"sources"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil || !res.Available || len(res.Sources) != 4 {
		t.Fatalf("decode: %v %s", err, rec.Body.String())
	}
	if res.Sources[0].Host != "celestrak.org" || res.Sources[0].State != "down" {
		t.Errorf("down source should come first: %+v", res.Sources[0])
	}
	if res.Sources[1].State != "failing" || res.Sources[1].Name != "Windows PC helper" {
		t.Errorf("PC helper: %+v", res.Sources[1])
	}
	for _, s := range res.Sources {
		if s.Host == "192.168.1.90" && s.Name != "Home network device (e.g. Home Assistant)" {
			t.Errorf("private host name: %q", s.Name)
		}
		if s.Host == "ll.thespacedevs.com" && (s.State != "ok" || s.Name == s.Host) {
			t.Errorf("launch library: %+v", s)
		}
	}
}
