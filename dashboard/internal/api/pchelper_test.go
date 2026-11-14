package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestPCHelperURLValidation(t *testing.T) {
	def := rendererSettingDefs["PC_HELPER_URL"]
	for _, c := range []struct {
		in   string
		want string
		ok   bool
	}{
		{"192.168.1.50:8765", "http://192.168.1.50:8765", true},
		{"http://desktop-6ikbi8t:8765/", "http://desktop-6ikbi8t:8765", true},
		{"", "", true},
		{"http://x:1/../etc", "", false},
		{"http://a b", "", false},
		{"file:///etc/passwd", "", false},
		{"https://host:443", "", false},
		{"http://h;rm -rf /", "", false},
	} {
		v := normalizeSetting("PC_HELPER_URL", c.in)
		if got := def.Valid(v); got != c.ok {
			t.Errorf("%q -> %q valid=%v, want %v", c.in, v, got, c.ok)
		}
		if c.ok && v != c.want {
			t.Errorf("%q normalised to %q, want %q", c.in, v, c.want)
		}
	}
}

func TestPCHelperStatus(t *testing.T) {
	pc := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/status.json" {
			http.NotFound(w, r)
			return
		}
		w.Write([]byte(`{"time":1,"jobs":{"nowcast":{"state":"ok"}}}`))
	}))
	defer pc.Close()
	dir := t.TempDir()
	old := rendererConfPath
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	defer func() { rendererConfPath = old }()

	get := func() map[string]any {
		rec := httptest.NewRecorder()
		GetPCHelperStatus()(rec, httptest.NewRequest(http.MethodGet, "/api/pchelper-status", nil))
		var m map[string]any
		json.Unmarshal(rec.Body.Bytes(), &m)
		return m
	}
	if m := get(); m["configured"] != false {
		t.Fatalf("unconfigured: %v", m)
	}
	os.WriteFile(rendererConfPath, []byte("PC_HELPER_URL="+pc.URL+"\n"), 0644)
	if m := get(); m["reachable"] != true {
		t.Fatalf("reachable: %v", m)
	}
	os.WriteFile(rendererConfPath, []byte("PC_HELPER_URL=http://127.0.0.1:1\n"), 0644)
	if m := get(); m["reachable"] != false {
		t.Fatalf("unreachable: %v", m)
	}
}
