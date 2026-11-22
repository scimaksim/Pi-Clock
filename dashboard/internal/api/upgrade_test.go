package api

import (
	"context"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestUpgradeStreamCompletion(t *testing.T) {
	for _, tc := range []struct {
		exit   string
		wantOK bool
	}{{"0", true}, {"1", false}} {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		cmd := exec.CommandContext(ctx, "sh", "-c",
			"echo STAGE:download; head -c 70000 /dev/zero >&2; echo >&2; echo STAGE:verify >&2; exit "+tc.exit)
		var stages []string
		err := streamUpgrade(cmd, func(s string) { stages = append(stages, s) })
		expired := ctx.Err()
		cancel()
		if expired != nil || (err == nil) != tc.wantOK || strings.Join(stages, ",") != "download,verify" {
			t.Fatalf("exit %s: stages %v, command %v, context %v", tc.exit, stages, err, expired)
		}
	}
}
