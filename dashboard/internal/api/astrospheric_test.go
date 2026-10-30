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
