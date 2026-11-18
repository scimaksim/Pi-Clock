package api

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A null, empty or unrelated body must never erase saved settings or a
// stored account.
func TestEmptyBodiesChangeNothing(t *testing.T) {
	dir := t.TempDir()
	layersConfPath = filepath.Join(dir, "layers.conf")
	spaceTrackPath = filepath.Join(dir, "st.cred")
	os.WriteFile(layersConfPath, []byte("borders=1,1.00\n"), 0644)
	os.WriteFile(spaceTrackPath, []byte("me@example.com\npw\n"), 0600)

	call := func(h http.HandlerFunc, body string) int {
		rec := httptest.NewRecorder()
		h(rec, httptest.NewRequest(http.MethodPut, "/", strings.NewReader(body)))
		return rec.Code
	}
	for _, body := range []string{`null`, `[]`, `{}`, `{"enabled":"yes"}`} {
		if c := call(PutLayers(), body); c != http.StatusBadRequest {
			t.Errorf("layers %s -> %d", body, c)
		}
		if c := call(PutSpaceTrack(), body); c != http.StatusBadRequest {
			t.Errorf("spacetrack %s -> %d", body, c)
		}
	}
	if d, _ := os.ReadFile(layersConfPath); string(d) != "borders=1,1.00\n" {
		t.Errorf("layers file changed: %q", d)
	}
	if d, _ := os.ReadFile(spaceTrackPath); string(d) != "me@example.com\npw\n" {
		t.Errorf("account file changed: %q", d)
	}
	// the explicit removal still works
	if c := call(PutSpaceTrack(), `{"identity":"","password":""}`); c != http.StatusOK {
		t.Errorf("explicit removal -> %d", c)
	}
}
