package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestSpaceTrackAccount(t *testing.T) {
	spaceTrackPath = filepath.Join(t.TempDir(), "st.cred")
	put := func(body string) int {
		rec := httptest.NewRecorder()
		PutSpaceTrack()(rec, httptest.NewRequest(http.MethodPut, "/", strings.NewReader(body)))
		return rec.Code
	}
	get := func() map[string]interface{} {
		rec := httptest.NewRecorder()
		GetSpaceTrack()(rec, httptest.NewRequest(http.MethodGet, "/", nil))
		var m map[string]interface{}
		json.Unmarshal(rec.Body.Bytes(), &m)
		return m
	}
	if put(`{"identity":"me@example.com","password":"p w&x"}`) != http.StatusOK {
		t.Fatal("save refused")
	}
	st, err := os.Stat(spaceTrackPath)
	if err != nil || st.Mode().Perm() != 0600 {
		t.Fatalf("file mode %v %v", st, err)
	}
	data, _ := os.ReadFile(spaceTrackPath)
	if string(data) != "me@example.com\np w&x\n" {
		t.Fatalf("stored %q", data)
	}
	m := get()
	if m["configured"] != true || m["user"] != "m…@example.com" {
		t.Fatalf("get %v", m)
	}
	for _, s := range []string{"pass", "p w", "example.com\n"} {
		if raw, _ := json.Marshal(m); strings.Contains(string(raw), s) && s != "example.com\n" {
			t.Fatalf("GET leaks %q", s)
		}
	}
	for _, bad := range []string{
		`{"identity":"me@example.com","password":"a\nb"}`,
		`{"identity":"me @x","password":"x"}`,
		`{"identity":"me@example.com","password":""}`,
	} {
		if put(bad) != http.StatusBadRequest {
			t.Fatalf("accepted %s", bad)
		}
	}
	if put(`{"identity":"","password":""}`) != http.StatusOK || get()["configured"] != false {
		t.Fatal("remove")
	}
}

func TestSpaceTrackSignIn(t *testing.T) {
	spaceTrackPath = filepath.Join(t.TempDir(), "st.cred")
	os.WriteFile(spaceTrackPath, []byte("me@example.com\ngood\n"), 0600)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		r.ParseForm()
		if r.URL.Path == "/ajaxauth/login" && r.Form.Get("password") == "good" {
			w.Write([]byte(`""`))
			return
		}
		w.Write([]byte(`{"Login":"Failed"}`))
	}))
	defer srv.Close()
	spaceTrackLoginURL = srv.URL + "/ajaxauth/login"
	run := func() int {
		rec := httptest.NewRecorder()
		TestSpaceTrack()(rec, httptest.NewRequest(http.MethodPost, "/", nil))
		return rec.Code
	}
	if c := run(); c != http.StatusOK {
		t.Fatalf("good account: %d", c)
	}
	if c := run(); c != http.StatusTooManyRequests {
		t.Fatalf("second test within a minute: %d", c)
	}
	stTestLast = time.Time{}
	os.WriteFile(spaceTrackPath, []byte("me@example.com\nbad\n"), 0600)
	if c := run(); c != http.StatusBadGateway {
		t.Fatalf("refused account: %d", c)
	}
}
