# demos/Makefile — orchestrates demo gif rendering.
#
#   make trace      Provision Hetzner VM, capture a trace, download to
#                   demos/local-trace/. Requires HCLOUD_TOKEN.
#   make dump-gif   Render demos/dump.gif from demos/local-trace via VHS.
#   make web-gif    Render demos/web.gif via Playwright.
#   make all        trace + dump-gif + web-gif.
#   make clean-vm   Delete the Hetzner VM (uses demos/.vm-id).
#   make clean      Remove local-trace/, .video/, node_modules/.
#
# Required tools: bash, rsync, ssh, scp, vhs (brew install vhs), node + npm,
# ffmpeg, jq, curl, brew install lz4 (macOS).

HERE := $(shell pwd)
REPO := $(abspath $(HERE)/..)
TRACE_DIR ?= $(HERE)/local-trace
export TRACE_DIR
# Recursive expansion reads metadata AFTER the trace recipe has written it.
PGWT_DEMO_CPUS = $(shell cat "$(TRACE_DIR)/capture-cpus.txt" 2>/dev/null)
export PGWT_DEMO_CPUS

.PHONY: all trace dump-gif web-gif clean clean-vm pgwt-server
.NOTPARALLEL:

all: trace dump-gif web-gif

# --- Build pgwt-server locally (needed for both gif renders) ---
pgwt-server:
	@$(HERE)/build-server-local.sh

# --- Capture a trace from a fresh Hetzner VM ---
trace:
	@$(HERE)/capture-trace.sh

# --- Render the dump.gif via VHS ---
# Tape runs from the repo root and inherits TRACE_DIR.
dump-gif: pgwt-server
	@command -v vhs >/dev/null || { echo "FATAL: vhs not installed (brew install vhs)"; exit 1; }
	@[ -d $(TRACE_DIR) ] || { echo "FATAL: $(TRACE_DIR) missing — run 'make trace'"; exit 1; }
	@grep -Eq '^[1-9][0-9]*$$' "$(TRACE_DIR)/capture-cpus.txt" || { echo "FATAL: capture CPU metadata missing — run 'make trace'"; exit 1; }
	@$(REPO)/pgwt-server --dump "$(TRACE_DIR)" > /dev/null
	cd $(REPO) && vhs demos/dump.tape

# --- Render the web.gif via Playwright ---
web-gif: pgwt-server pgwt-web
	@$(HERE)/record-web.sh

.PHONY: pgwt-web
pgwt-web:
	cd $(REPO) && $(MAKE) pgwt-client

# --- Delete the Hetzner VM ---
clean-vm:
	@if [ -f $(HERE)/.vm-id ]; then \
		ID=$$(cat $(HERE)/.vm-id); \
		echo "Deleting Hetzner VM $$ID ..."; \
		$(REPO)/tests/hetzner-vm.sh delete $$ID || exit $$?; \
		rm -f $(HERE)/.vm-id $(HERE)/.vm-ip; \
	else \
		echo "No demos/.vm-id — nothing to delete."; \
	fi

# --- Local cleanup (does NOT touch committed gifs) ---
clean:
	rm -rf $(TRACE_DIR) $(HERE)/.video $(HERE)/node_modules $(HERE)/.pgwt.log
	rm -f $(REPO)/pgwt-server $(REPO)/web/pgwt
