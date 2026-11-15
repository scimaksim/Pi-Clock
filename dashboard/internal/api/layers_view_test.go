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

func TestLayerViewRoundTrip(t *testing.T) {
	layersConfPath = filepath.Join(t.TempDir(), "layers.conf")
	body := `[{"name":"rivers","enabled":true,"opacity":1,"view":"local"},
	          {"name":"satclouds","enabled":true,"opacity":0.74,"view":"global"},
	          {"name":"radar","enabled":false,"opacity":0.8,"view":"bogus"}]`
	rec := httptest.NewRecorder()
	PutLayers()(rec, httptest.NewRequest(http.MethodPut, "/", strings.NewReader(body)))
	if rec.Code != 200 {
		t.Fatal(rec.Code)
	}
	data, _ := os.ReadFile(layersConfPath)
	for _, want := range []string{"rivers=1,1.00,2\n", "satclouds=1,0.74,1\n", "radar=0,0.80\n"} {
		if !strings.Contains(string(data), want) {
			t.Fatalf("missing %q in\n%s", want, data)
		}
	}
	rec = httptest.NewRecorder()
	GetLayers()(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	var got []map[string]interface{}
	json.Unmarshal(rec.Body.Bytes(), &got)
	views := map[string]string{}
	for _, l := range got {
		views[l["name"].(string)] = l["view"].(string)
	}
	if views["rivers"] != "local" || views["satclouds"] != "global" || views["radar"] != "any" || views["hud"] != "any" {
		t.Fatalf("views %v", views)
	}
}

func TestCoarseLayersFlagged(t *testing.T) {
	layersConfPath = filepath.Join(t.TempDir(), "layers.conf")
	rec := httptest.NewRecorder()
	GetLayers()(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	var got []map[string]interface{}
	json.Unmarshal(rec.Body.Bytes(), &got)
	coarse := map[string]bool{}
	for _, l := range got {
		if c, _ := l["coarse"].(bool); c {
			coarse[l["name"].(string)] = true
		}
	}
	for _, n := range []string{"wind", "cloud", "precip", "aurora", "bandconditions"} {
		if !coarse[n] {
			t.Fatalf("%s not flagged coarse: %v", n, coarse)
		}
	}
	if coarse["radar"] || coarse["satclouds"] || coarse["sst"] || len(coarse) != 5 {
		t.Fatalf("unexpected coarse set %v", coarse)
	}
}
