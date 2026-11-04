package api

import (
	"bufio"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

// healthPath is the renderer's vital-signs log (renderer/health.c).
var healthPath = "/data/cache/pi-clock-health.csv"

type healthSample struct {
	T      int64   `json:"t"`
	RSS    int64   `json:"rss_mb"`
	Avail  int64   `json:"avail_mb"`
	Temp   float64 `json:"temp_c"`
	CPU    float64 `json:"cpu_pct"`
	Load   float64 `json:"load1"`
	DataMB int64   `json:"data_free_mb"`
	TmpMB  int64   `json:"tmp_free_mb"`
	Uptime int64   `json:"uptime_s"`
}

type healthStart struct {
	T       int64  `json:"t"`
	Version string `json:"version"`
}

// GetHealth returns up to a week of samples (thinned to at most ~600
// points) plus the renderer start times, for the Health card.
func GetHealth() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		f, err := os.Open(healthPath)
		if err != nil {
			jsonResponse(w, http.StatusOK, map[string]any{"available": false, "now": time.Now().Unix()})
			return
		}
		defer f.Close()
		var samples []healthSample
		var starts []healthStart
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			p := strings.Split(strings.TrimSpace(sc.Text()), ",")
			switch {
			case len(p) >= 3 && p[0] == "start":
				t, _ := strconv.ParseInt(p[1], 10, 64)
				starts = append(starts, healthStart{T: t, Version: p[2]})
			case len(p) >= 10 && p[0] == "t":
				var s healthSample
				s.T, _ = strconv.ParseInt(p[1], 10, 64)
				s.RSS, _ = strconv.ParseInt(p[2], 10, 64)
				s.Avail, _ = strconv.ParseInt(p[3], 10, 64)
				s.Temp, _ = strconv.ParseFloat(p[4], 64)
				s.CPU, _ = strconv.ParseFloat(p[5], 64)
				s.Load, _ = strconv.ParseFloat(p[6], 64)
				s.DataMB, _ = strconv.ParseInt(p[7], 10, 64)
				s.TmpMB, _ = strconv.ParseInt(p[8], 10, 64)
				s.Uptime, _ = strconv.ParseInt(p[9], 10, 64)
				samples = append(samples, s)
			}
		}
		// Thin to ~600 points so the page stays light
		if n := len(samples); n > 600 {
			step := (n + 599) / 600
			var thin []healthSample
			for i := 0; i < n; i += step {
				thin = append(thin, samples[i])
			}
			thin = append(thin, samples[n-1])
			samples = thin
		}
		now := time.Now().Unix()
		restarts24 := 0
		for _, s := range starts {
			if now-s.T < 86400 {
				restarts24++
			}
		}
		jsonResponse(w, http.StatusOK, map[string]any{
			"available": true, "now": now, "samples": samples, "starts": starts,
			"restarts_24h": restarts24,
		})
	}
}
