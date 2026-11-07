"""Pi-Clock PC helper: runs the heavy jobs and serves their output.

    python -m pchelper.server [--config helper.json] [--once JOB]

Each job (nowcast, hrrr, birds) runs in its own thread: run() returns
the seconds until its next run; an error is logged and retried with a
growing back-off. The output folder is served read-only over HTTP on
the LAN (default port 8765) for the clock to fetch, with /status.json
summarising the jobs.
"""

import argparse
import functools
import http.server
import importlib
import json
import logging
import logging.handlers
import os
import socketserver
import sys
import threading
import time

from .common import load_config

log = logging.getLogger("pchelper")
STATUS = {}
_lock = threading.Lock()


def _set(job, **kw):
    with _lock:
        STATUS.setdefault(job, {}).update(kw)


def job_loop(cfg, name, stop):
    backoff = 60
    try:
        mod = importlib.import_module(f"pchelper.{name}")
    except Exception as e:                       # missing library etc.
        log.exception("%s: cannot load", name)
        _set(name, state="unavailable", error=f"{type(e).__name__}: {e}")
        return
    while not stop.is_set():
        t = time.time()
        _set(name, state="running", started=int(t))
        try:
            wait = int(mod.run(cfg))
            _set(name, state="ok", last_ok=int(time.time()), seconds=round(time.time() - t, 1),
                 error=None, next=int(time.time() + wait))
            backoff = 60
        except Exception as e:
            log.exception("%s failed", name)
            wait = backoff
            backoff = min(backoff * 2, 1800)
            _set(name, state="error", error=f"{type(e).__name__}: {e}"[:300],
                 last_error=int(time.time()), next=int(time.time() + wait))
        stop.wait(wait)


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):          # quiet
        pass

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if self.path.split("?")[0] == "/status.json":
            with _lock:
                body = json.dumps({"time": int(time.time()), "jobs": STATUS}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        self.send_error(405)

    do_PUT = do_DELETE = do_POST


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--config", default=os.path.join(root, "helper.json"))
    ap.add_argument("--once", help="run one job once and exit (testing)")
    args = ap.parse_args()
    cfg = load_config(args.config)

    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    logging.getLogger().setLevel(logging.INFO)
    fh = logging.handlers.RotatingFileHandler(os.path.join(root, "helper.log"),
                                              maxBytes=2_000_000, backupCount=2,
                                              encoding="utf-8")
    fh.setFormatter(fmt)
    logging.getLogger().addHandler(fh)
    sh = logging.StreamHandler(sys.stdout)
    sh.setFormatter(fmt)
    logging.getLogger().addHandler(sh)

    # Stay out of the way of interactive use (still above BOINC's idle class)
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.kernel32.SetPriorityClass(
                ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)   # BELOW_NORMAL
        except Exception:
            pass
    else:
        try:
            os.nice(5)
        except OSError:
            pass
    try:
        import torch
        torch.set_num_threads(max(1, (os.cpu_count() or 4) // 2))
    except Exception:
        pass

    if args.once:
        mod = importlib.import_module(f"pchelper.{args.once}")
        print("next run in", mod.run(cfg), "s")
        return

    stop = threading.Event()
    for name in cfg["jobs"]:
        threading.Thread(target=job_loop, args=(cfg, name, stop), daemon=True,
                         name=name).start()
    handler = functools.partial(Handler, directory=cfg["out_dir"])
    srv = Server(("0.0.0.0", int(cfg["port"])), handler)
    log.info("serving %s on port %s (jobs: %s)", cfg["out_dir"], cfg["port"],
             ", ".join(cfg["jobs"]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        srv.server_close()


if __name__ == "__main__":
    main()
