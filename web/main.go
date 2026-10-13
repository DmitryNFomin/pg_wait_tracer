package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"runtime/debug"
	"sort"
)

//go:embed static
var staticFiles embed.FS

// Version handshake (Trust Milestone T7 / TST-11). A skewed client/server
// pair (client rebuilt on the laptop, server deployed weeks ago — or vice
// versa) is the NORMAL deployment state; the handshake makes the skew VISIBLE
// (bridge log + UI banner) without refusing to work.
//
// buildVersion is embedded at build time via `make pgwt-client`
// (-ldflags "-X main.buildVersion=$(git describe ...)") so it matches the
// version string the C binaries embed. protocolRev must stay in sync with
// PGWT_PROTOCOL_REV in src/pg_wait_tracer.h (and tests/mock_server.py).
var buildVersion = "dev"

const protocolRev = 1

// clientVersion resolves the version to report: the linker-embedded one, or
// (for plain `go build` without ldflags) the VCS revision Go stamps into the
// binary, or "dev" when neither exists.
func clientVersion() string {
	if buildVersion != "dev" {
		return buildVersion
	}
	if info, ok := debug.ReadBuildInfo(); ok {
		rev, dirty := "", ""
		for _, s := range info.Settings {
			switch s.Key {
			case "vcs.revision":
				rev = s.Value
			case "vcs.modified":
				if s.Value == "true" {
					dirty = "-dirty"
				}
			}
		}
		if len(rev) >= 7 {
			return "dev-" + rev[:7] + dirty
		}
	}
	return "dev"
}

func main() {
	port := flag.Int("port", 8384, "local HTTP port")
	traceDir := flag.String("trace-dir", "/var/lib/pgsql/18/data/pg_wait_tracer/", "remote trace directory")
	serverPath := flag.String("server-path", "pgwt-server", "remote pgwt-server binary path")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: pgwt [flags] user@host\n\n")
		fmt.Fprintf(os.Stderr, "Connects to a PostgreSQL server over SSH and opens\n")
		fmt.Fprintf(os.Stderr, "a wait event investigation UI in the browser.\n\n")
		fmt.Fprintf(os.Stderr, "Flags:\n")
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\nExamples:\n")
		fmt.Fprintf(os.Stderr, "  pgwt root@db-server\n")
		fmt.Fprintf(os.Stderr, "  pgwt --trace-dir /tmp/traces root@db-server\n")
		fmt.Fprintf(os.Stderr, "  pgwt --server-path /root/pg_wait_tracer/pgwt-server root@db-server\n")
	}
	flag.Parse()

	if flag.NArg() < 1 {
		flag.Usage()
		os.Exit(1)
	}
	host := flag.Arg(0)

	// UI-12: per-session WebSocket token.
	//
	// Threat model: this WebSocket can escalate a PRODUCTION daemon to
	// full-fidelity capture, so it must not be reachable by arbitrary web
	// pages. The localhost Origin check blocks pages served from other
	// origins (including DNS-rebinding pages, whose Origin stays the
	// attacker's hostname), but Origin is absent for non-browser clients, so
	// alone it is not enough. The token is a per-process random capability:
	// it is served only at /session on this localhost server and the page we
	// serve fetches it same-origin — a foreign page cannot read that response.
	// LIMIT: any process that can already reach localhost:PORT as this user
	// can fetch the token too; this is a same-user capability, not
	// authentication across local users or a substitute for SSH security.
	token := newSessionToken()

	// Spawn SSH → pgwt-server on remote host (supervised: respawns with
	// backoff if the SSH session or the server dies — UI-3). The bridge also
	// runs the version handshake against every (re)spawned server and warns
	// on stderr when the pair is skewed (T7 / TST-11).
	version := clientVersion()
	log.Printf("pgwt %s (protocol %d)", version, protocolRev)
	bridge := NewSSHBridge(host, *serverPath, *traceDir, token, version)
	if err := bridge.Start(); err != nil {
		log.Fatalf("Failed to start: %v", err)
	}
	defer bridge.Close()
	log.Printf("SSH connected to %s", host)

	// WebSocket endpoint (token-gated).
	http.HandleFunc("/ws", bridge.HandleWebSocket)

	// Session endpoint: same-origin readable only (no CORS headers). Besides
	// the WS token it reports the client's build version + protocol revision
	// so the UI can compare them against the server's `info` response and
	// show a version-skew banner (T7 / TST-11).
	http.HandleFunc("/session", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(map[string]interface{}{
			"token":          token,
			"client_version": version,
			"protocol":       protocolRev,
		})
	})

	// Serve embedded static files.
	sub, err := fs.Sub(staticFiles, "static")
	if err != nil {
		// Can only happen if the //go:embed directive above is broken, which
		// is a build-time certainty rather than a runtime risk -- but serving
		// a UI-less bridge is worse than refusing to start.
		log.Fatalf("embedded static FS unavailable: %v", err)
	}

	// Embedded-asset manifest: the paths this BINARY actually carries.
	//
	// Why it exists: web/static is compiled into this binary, so a bridge
	// built before a UI edit serves the old assets with no outward sign, and
	// a live-UI-smoke walk or a counted demo rehearsal then grades the wrong
	// UI and reports green. tests/live_daemon_lib.sh's
	// assert_served_ui_matches_tree compares what is served against the tree
	// under test -- and to do that it has to know WHICH files the binary was
	// built with. Deriving that list by re-implementing Go's //go:embed
	// exclusion rules in the test (dot/underscore prefixes, symlinks, nested
	// modules, go.mod as a file vs a directory, dangling symlinks) produced
	// three separate silent passes in review, each one a gap between the
	// replica and the real rules. So the binary answers for itself.
	//
	// Paths only. No contents (the FileServer below already serves those),
	// no filesystem outside the embedded FS -- embed.FS cannot reference
	// anything else, so there is no traversal to guard against. Not
	// token-gated, because it discloses nothing new: http.FileServer already
	// serves a directory listing for every embedded directory without an
	// index.html (measured: GET /lib/ returns an HTML index of camera.js,
	// control.js, format.js, ...), so the path set is already enumerable by
	// anyone who can reach this port. This endpoint only saves the guard from
	// scraping those listings.
	//
	// Deliberately NOT behind a build tag or a flag: a guard that works only
	// in a special build is a guard that will not run when it matters, and
	// the one thing it must catch is the case where someone forgot a step.
	http.HandleFunc("/embedded-assets", func(w http.ResponseWriter, r *http.Request) {
		paths := []string{}
		err := fs.WalkDir(sub, ".", func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if !d.IsDir() {
				paths = append(paths, p)
			}
			return nil
		})
		if err != nil {
			// A partial list would be worse than none: the guard treats a
			// failed fetch as "cannot verify" and refuses, which is correct,
			// whereas a short list would look like a smaller tree.
			http.Error(w, "walking embedded FS: "+err.Error(),
				http.StatusInternalServerError)
			return
		}
		sort.Strings(paths)
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(map[string]interface{}{
			"paths":          paths,
			"count":          len(paths),
			"client_version": version,
		})
	})

	http.Handle("/", http.FileServer(http.FS(sub)))

	addr := fmt.Sprintf("localhost:%d", *port)
	log.Printf("Open http://%s in your browser", addr)
	openBrowser("http://" + addr)

	// Graceful shutdown on Ctrl+C.
	go func() {
		c := make(chan os.Signal, 1)
		signal.Notify(c, os.Interrupt)
		<-c
		log.Println("Shutting down...")
		bridge.Close()
		os.Exit(0)
	}()

	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatal(err)
	}
}

func newSessionToken() string {
	buf := make([]byte, 16)
	if _, err := rand.Read(buf); err != nil {
		log.Fatalf("session token: %v", err)
	}
	return hex.EncodeToString(buf)
}

func openBrowser(url string) {
	switch runtime.GOOS {
	case "darwin":
		exec.Command("open", url).Start()
	default:
		exec.Command("xdg-open", url).Start()
	}
}
