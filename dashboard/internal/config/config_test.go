package config

import (
	"path/filepath"
	"testing"
)

func TestFailedUpdatePreservesSettings(t *testing.T) {
	oldPath := rendererConfPath
	t.Cleanup(func() { rendererConfPath = oldPath })
	dir := t.TempDir()
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	cfg := Load()
	if err := cfg.Update(map[string]string{"callsign": "OLD"}); err != nil {
		t.Fatal(err)
	}
	if err := cfg.Update(map[string]string{"callsign": "BAD", "map_zoom": "invalid"}); err == nil {
		t.Fatal("invalid zoom accepted")
	}
	if got := cfg.Snapshot().Callsign; got != "OLD" {
		t.Fatalf("validation changed callsign to %q", got)
	}
	rendererConfPath = dir // a directory cannot be read as a settings file
	if err := cfg.Update(map[string]string{"callsign": "BAD"}); err == nil {
		t.Fatal("expected file error")
	}
	rendererConfPath = filepath.Join(dir, "renderer.conf")
	if err := cfg.Update(map[string]string{"map_zoom": "10"}); err != nil {
		t.Fatal(err)
	}
	if got := Load().Snapshot().Callsign; got != "OLD" {
		t.Fatalf("later save persisted rejected callsign %q", got)
	}
}

func TestGridSquareMustBeALocator(t *testing.T) {
	oldPath := rendererConfPath
	t.Cleanup(func() { rendererConfPath = oldPath })
	rendererConfPath = filepath.Join(t.TempDir(), "renderer.conf")
	cfg := Load()
	for _, ok := range []string{"FM05pq", "fm05", "AJ00aa", "RR99xx", ""} {
		if err := cfg.Update(map[string]string{"grid_square": ok}); err != nil {
			t.Fatalf("%q rejected: %v", ok, err)
		}
	}
	for _, bad := range []string{"SJ00aa", "JS00aa", "FM05py", "FM5", "FM05pq12"} {
		if err := cfg.Update(map[string]string{"grid_square": bad}); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
	if got := cfg.Snapshot().GridSquare; got != "" {
		t.Fatalf("grid %q after rejected saves", got)
	}
}
