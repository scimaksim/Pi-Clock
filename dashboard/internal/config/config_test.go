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
