package api

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// Overlapping saves of one file must each succeed and leave a complete
// copy of one request, never a mix or a missing file.
func TestAtomicWriteConcurrentSaves(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "pi-clock-layers.conf")
	bodies := make([][]byte, 16)
	for i := range bodies {
		bodies[i] = bytes.Repeat([]byte{byte('a' + i)}, 256*1024)
	}
	var wg sync.WaitGroup
	errs := make(chan error, len(bodies)*8)
	for round := 0; round < 8; round++ {
		for i := range bodies {
			wg.Add(1)
			go func(b []byte) {
				defer wg.Done()
				if err := atomicWriteFile(path, b, 0600); err != nil {
					errs <- err
				}
			}(bodies[i])
		}
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("concurrent save failed: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	complete := false
	for _, b := range bodies {
		complete = complete || bytes.Equal(got, b)
	}
	if !complete {
		t.Fatalf("file holds a mix of requests (%d bytes)", len(got))
	}
	if st, _ := os.Stat(path); st.Mode().Perm() != 0600 {
		t.Fatalf("mode %v, want 0600", st.Mode().Perm())
	}
	left, _ := os.ReadDir(dir)
	for _, e := range left {
		if strings.Contains(e.Name(), ".tmp") {
			t.Fatalf("temporary file left behind: %s", e.Name())
		}
	}
}
