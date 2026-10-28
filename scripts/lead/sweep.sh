#!/usr/bin/env bash
# sweep.sh -- the reconciler. Joins GitHub, git and local state and prints the
# items the lead must act on.
#
# DESIGN RULES, each one learned by getting it wrong:
#  * A failed or incomplete query prints UNKNOWN. Never FREE, never silence.
#  * Branch EVIDENCE and ownership count -- never a branch NAME, never mere PR
#    existence. Squash-merge and integration PRs both destroy ancestry, so no
#    branch-level query can tell "stranded" from "landed by another route".
#    Those lines are CANDIDATEs for a content check, never conclusions.
#  * BULK queries only. The first version issued one `gh pr list --head` per
#    branch -- 302 branches x 0.52s = 2.6 min, which forced a background refresh,
#    which could not be detached, which cost four failed attempts. One bulk call
#    returns all PRs in ~1.7s. Speed here is not an optimisation; it is what lets
#    the hook run this synchronously and removes the whole class of problem.
export GH_PAGER=cat; set -u
# OUTDIR is where side-effect files (currently just .dispatch-queue) are
# written. It defaults to this script's own directory so a manual run from
# any worktree does something sane, but the hook ALWAYS passes its own $D
# explicitly: git worktrees share one .git, so the hardcoded `cd` below is
# fine for the git/gh queries, but each worktree's hook keeps its own
# .sweep-latest.txt cache (via stdout redirect in sweep-hook.sh) and the
# dispatch queue must land next to THAT cache, not next to whichever
# worktree happened to be $0's directory.
OUTDIR="${1:-$(cd "$(dirname "$0")" && pwd)}"
SELFDIR="$(cd "$(dirname "$0")" && pwd)"
cd /Users/dmitryfomin/work/git/pg_wait_tracer
RUN="sweep-$(date -u +%Y%m%dT%H%M%SZ)"
git fetch origin -q 2>/dev/null || { echo "$RUN UNKNOWN: git fetch failed"; exit 3; }
echo "== $RUN  master=$(git rev-parse --short origin/master) =="

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# ---- three bulk calls, the whole GitHub side -------------------------------
gh pr list --state all --limit 700 --json number,state,headRefName > "$TMP/prs.json" 2>/dev/null \
  || { echo "UNKNOWN: gh pr list failed -- no claim may be made"; exit 3; }
[ -s "$TMP/prs.json" ] || { echo "UNKNOWN: gh returned no PRs (auth/API failure, not an empty repo)"; exit 3; }
gh pr list --state open --limit 100 --json number,mergeStateStatus,autoMergeRequest,headRefName > "$TMP/open.json" 2>/dev/null || echo "UNKNOWN: open-PR query failed"
gh issue list --state open --limit 400 --json number > "$TMP/issues.json" 2>/dev/null || echo "UNKNOWN: issue query failed"
gh pr list --state merged --limit 10 --json number,title,body > "$TMP/merged.json" 2>/dev/null || echo "UNKNOWN: merged-PR query failed"

# ---- [1] recently merged PR that mentions a still-open issue ---------------
echo "-- [1] merged PR mentions an open issue (CANDIDATES: a mention is not a fix)"
/usr/bin/python3 - "$TMP" <<'PY'
import json,re,sys
t=sys.argv[1]
try:
    openi={str(i["number"]) for i in json.load(open(t+"/issues.json"))}
    merged=json.load(open(t+"/merged.json"))
except Exception as e:
    print(f"   UNKNOWN: cannot join merged PRs to open issues ({e})"); raise SystemExit
for pr in merged:
    for n in sorted(set(re.findall(r"#(\d{2,4})", (pr.get("title") or "")+" "+(pr.get("body") or "")))):
        if n in openi and n != str(pr["number"]):
            print(f"   CANDIDATE: PR #{pr['number']} merged mentions #{n}, still OPEN -- verify content on master")
PY

# ---- [2] branch evidence vs PR state, from the bulk map --------------------
echo "-- [2] branch evidence vs PR state"
live_wt=$(git worktree list --porcelain 2>/dev/null | /usr/bin/awk '/^branch /{sub("refs/heads/","",$2); print $2}')
printf '%s\n' "$live_wt" > "$TMP/live_wt.txt"
/usr/bin/python3 - "$TMP" <<'PY' > "$TMP/prmap"
import json,sys
m={}
for p in json.load(open(sys.argv[1]+"/prs.json")):
    m.setdefault(p["headRefName"], (p["number"], p["state"]))
for b,(n,s) in m.items(): print(f"{b}\t{n}\t{s}")
PY
for b in $(git branch --list 'agent/*' --format='%(refname:short)'); do
  mb=$(git merge-base "$b" origin/master 2>/dev/null) || { echo "   UNKNOWN: no merge-base for $b"; continue; }
  ahead=$(git rev-list --count "$mb..$b" 2>/dev/null)
  row=$(/usr/bin/awk -F'\t' -v B="$b" '$1==B{print $2" "$3}' "$TMP/prmap")
  printf '%s\n' "$live_wt" | /usr/bin/grep -qx "$b" && live=" [LIVE WORKTREE]" || live=""
  if [ "$ahead" = "0" ]; then
    [ -n "$live" ] || echo "   FINDING: $b is ZERO-AHEAD -- empty shell, not work in progress"
  elif [ -z "$row" ]; then
    echo "   CANDIDATE: $b has $ahead commits and NO PR ever -- verify content on master"
  elif [ "${row#* }" = "CLOSED" ]; then
    echo "   CANDIDATE: $b has $ahead commits, PR ${row%% *} CLOSED-not-merged -- verify content on master"
  fi
done | head -25

# ---- [3] armed but BEHIND: these never merge on their own -------------------
echo "-- [3] armed but BEHIND (auto-merge never updates a behind branch)"
/usr/bin/python3 - "$TMP" <<'PY'
import json,sys
try: rows=json.load(open(sys.argv[1]+"/open.json"))
except Exception as e: print(f"   UNKNOWN: open-PR state unavailable ({e})"); raise SystemExit
for p in rows:
    if p.get("mergeStateStatus")=="BEHIND" and p.get("autoMergeRequest"):
        print(f"   FINDING: #{p['number']} BEHIND+ARMED {p['headRefName']} -- needs gh pr update-branch")
PY

# ---- [4] file reservations -------------------------------------------------
echo "-- [4] blocker files: run scripts/lead/held.sh; a missing declaration is UNKNOWN, not FREE"

# ---- [5] live VMs ----------------------------------------------------------
echo "-- [5] VMs alive"
( export HCLOUD_TOKEN="$(security find-generic-password -s hcloud -a claude_token -w 2>/dev/null)"
  [ -n "${HCLOUD_TOKEN:-}" ] || { echo "   UNKNOWN: no Hetzner token"; exit 0; }
  tests/hetzner-vm.sh list 2>/dev/null | /usr/bin/sed 's/^/   /' || echo "   UNKNOWN: hcloud list failed" )

# ---- [6] top tag-blocker issues READY-NOT-STARTED --------------------------
# The eleven-hour quantity: an issue nobody will see as "blocked" because
# nothing is blocking it -- it just has no open PR and no live worktree.
# A failed join degrades to UNKNOWN, never to "nothing is ready" -- see [1].
echo "-- [6] top tag-blocker issues READY-NOT-STARTED (no open PR, no live worktree)"
gh issue list -l tag-blocker --state open --limit 400 --json number,title > "$TMP/blockers.json" 2>/dev/null \
  || echo "   UNKNOWN: tag-blocker issue query failed"
if [ -s "$TMP/blockers.json" ]; then
  gh pr list --state open --limit 100 --json number,title,body,closingIssuesReferences > "$TMP/open_full.json" 2>/dev/null \
    || echo "   UNKNOWN: open-PR detail query failed -- cannot clear any issue as covered"
  /usr/bin/python3 - "$TMP" <<'PY'
import json,re,sys
t=sys.argv[1]
try:
    issues=json.load(open(t+"/blockers.json"))
except Exception as e:
    print(f"   UNKNOWN: cannot read tag-blocker issues ({e})"); raise SystemExit
try:
    prs=json.load(open(t+"/open_full.json"))
except Exception:
    prs=None  # the UNKNOWN line above already fired; join degrades below
try:
    live=[l.strip() for l in open(t+"/live_wt.txt") if l.strip()]
except Exception:
    live=[]

def pr_covers(n):
    if prs is None:
        return None
    ns=str(n)
    for p in prs:
        refs={str(r.get("number")) for r in (p.get("closingIssuesReferences") or [])}
        if ns in refs: return True
        blob=(p.get("title") or "")+" "+(p.get("body") or "")
        if re.search(r'#'+ns+r'\b', blob): return True
    return False

def wt_covers(n):
    ns=str(n)
    pat=re.compile(r'(?<!\d)'+ns+r'(?!\d)')
    return any(pat.search(b) for b in live)

shown=0
for i in sorted(issues, key=lambda x: -x["number"]):
    n=i["number"]
    covered=pr_covers(n)
    if covered is None:
        print(f"   UNKNOWN: issue #{n} PR coverage undetermined (open-PR query failed)")
        continue
    if covered or wt_covers(n):
        continue
    title=(i.get("title") or "")[:70]
    print(f'   FINDING: issue #{n} is READY-NOT-STARTED -- no open PR, no live worktree ("{title}")')
    shown+=1
    if shown>=5: break
PY
fi

# ---- [7] box occupancy ------------------------------------------------------
# Degrades to UNKNOWN on every failure path: unset var, unreachable host, or a
# missing ssh binary. Never prints anything that could be read as "free".
echo "-- [7] box occupancy"
if ! command -v ssh >/dev/null 2>&1; then
  echo "   UNKNOWN: ssh not available"
else
  for var in PGWT_BOX PGWT_BOX_EL8; do
    box="${!var:-}"
    if [ -z "$box" ]; then
      echo "   UNKNOWN: $var not set"
      continue
    fi
    out=$(ssh -o BatchMode=yes -o ConnectTimeout=3 -o StrictHostKeyChecking=no "$box" \
          'echo LOCK:; fuser /tmp/pgwt-box-check.lock 2>&1; echo UPTIME:; uptime' 2>&1)
    rc=$?
    if [ $rc -ne 0 ] || [ -z "$out" ]; then
      echo "   UNKNOWN: $var ssh failed (rc=$rc)"
    else
      echo "   BOX $var: $(printf '%s' "$out" | tr '\n' ' ')"
    fi
  done
fi

# ---- [8] codex liveness -----------------------------------------------------
# pgrep already excludes its own pid; nothing here greps for a pattern that
# could appear in this very script's own invocation line.
echo "-- [8] codex liveness"
if ! command -v pgrep >/dev/null 2>&1; then
  echo "   UNKNOWN: pgrep not available"
else
  hits=$(pgrep -f 'codex exec' 2>/dev/null || true)
  if [ -n "$hits" ]; then
    n=$(printf '%s\n' "$hits" | /usr/bin/grep -c .)
    echo "   CODEX: running ($n pid(s): $(printf '%s' "$hits" | tr '\n' ' '))"
  else
    echo "   CODEX: idle"
  fi
fi

# ---- [9] dispatch queue: the escalating-obligation gate reads this file ----
# Ready == open tag-blocker issue, no open PR, no live worktree -- the exact
# quantity in [6], but written WITHOUT the "show top 5" cap that section uses
# for the lead's own reading, because the escalation gate must be able to see
# an item even when it isn't in the top 5 by issue number.
#
# STATUS OK / STATUS UNKNOWN on line 2 are the whole contract: an empty ready
# list and a failed join must never produce the same file shape. The consumer
# (sweep-hook.sh) refuses to read anything else as "confirmed empty".
#
# Counts persist only across a run whose PREVIOUS file was STATUS OK; a gap
# (missing file, or the prior run was itself UNKNOWN) resets the streak to 1
# rather than guessing continuity through a period this script could not see.
#
# `running`: a ledger of actual SubagentStart/SubagentStop pairs (see
# agent-ledger.py), NOT a count of live agent/* worktrees -- that proxy read
# 227 against a cap of 3 on this repo (a worktree is kept until its PR opens,
# so hundreds of historical ones survive), which made `running >= cap`
# permanently true and permanently paused escalation. Before trusting it,
# SANITY_MAX below refuses an implausible count outright (STATUS UNKNOWN,
# not a quiet pass) rather than writing a running/cap pair nobody could act
# on: an absurd answer is not a clear answer, the same rule this project
# already applies to every other probe in this file.
DQ="$OUTDIR/.dispatch-queue"
echo "-- [9] dispatch queue -> $DQ"
LEDGER="/Users/dmitryfomin/work/git/pg_wait_tracer/scripts/lead/.active-agents"
SANITY_MAX=20   # generous over the 3-agent cap (reviewers/advisers share the ledger too)
RUNNING_N=$(/usr/bin/python3 "$SELFDIR/agent-ledger.py" count "$LEDGER" 2>/dev/null)

# ---- independent corroboration for a LOW running count ---------------------
# SANITY_MAX above catches "too high" (227 against a cap of 3). This is the
# mirror and the more dangerous direction: "too low against evidence to the
# contrary" fires escalation hardest exactly when the ledger knows least --
# cold start, before any already-running agent has ever lived through a
# fresh SubagentStart/Stop pair to populate it. Checked only when the ledger
# says running==0, against THREE independent signals, ORed together:
#   - a `make check` actually holding the machine-wide lock RIGHT NOW
#     (scripts/check-lock.py processes -- pgrep, no portability concern)
#   - a worktree's own git ADMIN dir touched recently
#     (.git/worktrees/<name>/HEAD -- moves on commit/checkout)
#   - a worktree's own checkout directory touched recently
#     (.claude/worktrees/agent-*, top-level mtime)
# An earlier version used HEAD-mtime only, on the claim "directory-mtime read
# zero" -- that claim was never actually measured: the first attempt used
# `find -newermt '-2 hours'`, which this host's `find` (bfs) rejects outright
# (it requires an absolute timestamp), so the command silently produced no
# output under `2>/dev/null` and looked like a zero count. Measured properly
# (absolute cutoff) on this repo: directory-mtime reads 9, not 0 -- a real,
# nonzero, independent signal, the opposite of the retracted claim. Worse,
# review found the soundness case live: HEAD only moves on commit/checkout,
# so a long build-and-test stretch with no commits lets it go stale while the
# agent is plainly alive -- one check-lock-holding agent's directory mtime
# was 90 minutes fresher than its HEAD mtime. So both are ORed in, not just
# HEAD: the costs here are asymmetric by design -- a false UNKNOWN delays
# escalation by one scan (>=10 min), while a false "confirmed running=0" is
# the over-cap-start catastrophe this guard exists to prevent, so the guard
# errs toward refusing whenever ANY corroborating signal disagrees with 0.
CHECK_PROCS=$(pgrep -f 'scripts/check-lock.py' 2>/dev/null | wc -l | tr -d ' ')
WT_CUTOFF=$(date -v-2H +"%Y-%m-%dT%H:%M:%S" 2>/dev/null)
if [ -n "$WT_CUTOFF" ]; then
  RECENT_WT_HEAD=$(find .git/worktrees -maxdepth 2 -name 'HEAD' -newermt "$WT_CUTOFF" 2>/dev/null | wc -l | tr -d ' ')
  RECENT_WT_DIR=$(find .claude/worktrees -maxdepth 1 -type d -name 'agent-*' -newermt "$WT_CUTOFF" 2>/dev/null | wc -l | tr -d ' ')
else
  RECENT_WT_HEAD=""   # `date -v` unsupported on this host -- unknown, not zero
  RECENT_WT_DIR=""
fi

/usr/bin/python3 - "$TMP" "$DQ" "$RUNNING_N" "$SANITY_MAX" "$CHECK_PROCS" "$RECENT_WT_HEAD" "$RECENT_WT_DIR" <<'PY'
import json, os, re, sys
t, dq, running_raw, sanity_max = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
check_procs_raw, recent_wt_head_raw, recent_wt_dir_raw = sys.argv[5], sys.argv[6], sys.argv[7]

def write_unknown(reason):
    tmp = dq + ".tmp"
    with open(tmp, "w") as f:
        f.write("# dispatch-queue v1\n")
        f.write(f"STATUS UNKNOWN {reason}\n")
    os.replace(tmp, dq)
    print(f"   UNKNOWN: dispatch queue not updated ({reason})")

try:
    running = int(running_raw)
    if running < 0:
        raise ValueError("negative")
except (ValueError, TypeError):
    write_unknown(f"running-agent count unreadable ({running_raw!r})"); raise SystemExit
if running > sanity_max:
    write_unknown(
        f"running-agent count {running} is implausible for a 3-agent-cap "
        f"project (> SANITY_MAX={sanity_max}) -- refusing to trust the proxy "
        f"rather than writing a running/cap pair nobody could act on"
    ); raise SystemExit

def _int_or_none(s):
    try:
        return int(s)
    except (ValueError, TypeError):
        return None

check_procs = _int_or_none(check_procs_raw) or 0
recent_wt_head = _int_or_none(recent_wt_head_raw)  # None if `date -v` unavailable
recent_wt_dir = _int_or_none(recent_wt_dir_raw)    # None if `date -v` unavailable
if running == 0 and (
    check_procs > 0 or (recent_wt_head or 0) > 0 or (recent_wt_dir or 0) > 0
):
    write_unknown(
        f"running-agent ledger reports 0 but independent evidence disagrees "
        f"(check-lock processes={check_procs}, worktrees with HEAD touched "
        f"<2h={'n/a' if recent_wt_head is None else recent_wt_head}, worktree "
        f"dirs touched <2h={'n/a' if recent_wt_dir is None else recent_wt_dir}) "
        f"-- a cold-start ledger against contrary evidence is a disagreement, "
        f"not an answer"
    ); raise SystemExit

try:
    issues = json.load(open(t + "/blockers.json"))
except Exception as e:
    write_unknown(f"cannot read tag-blocker issues: {e}"); raise SystemExit
try:
    prs = json.load(open(t + "/open_full.json"))
except Exception as e:
    write_unknown(f"open-PR detail query failed: {e}"); raise SystemExit
try:
    live = [l.strip() for l in open(t + "/live_wt.txt") if l.strip()]
except Exception:
    live = []

def pr_covers(n):
    ns = str(n)
    for p in prs:
        refs = {str(r.get("number")) for r in (p.get("closingIssuesReferences") or [])}
        if ns in refs:
            return True
        blob = (p.get("title") or "") + " " + (p.get("body") or "")
        if re.search(r'#' + ns + r'\b', blob):
            return True
    return False

def wt_covers(n):
    ns = str(n)
    pat = re.compile(r'(?<!\d)' + ns + r'(?!\d)')
    return any(pat.search(b) for b in live)

ready = []
for i in issues:
    n = i["number"]
    if pr_covers(n) or wt_covers(n):
        continue
    ready.append((n, (i.get("title") or "").replace("\n", " ")[:70]))

old_counts = {}
try:
    with open(dq) as f:
        old_lines = f.read().splitlines()
    if len(old_lines) >= 2 and old_lines[1].strip() == "STATUS OK":
        for ln in old_lines[2:]:
            if not ln.startswith("issue-"):
                continue
            parts = ln.split(" ", 3)
            if len(parts) < 3:
                continue
            try:
                old_counts[parts[0]] = int(parts[2])
            except ValueError:
                continue
except FileNotFoundError:
    pass

tmp = dq + ".tmp"
with open(tmp, "w") as f:
    f.write("# dispatch-queue v1  (id issue# consecutive-ready-scans title...)\n")
    f.write("STATUS OK\n")
    f.write(f"CAP running={running} cap=3\n")  # 3 == CLAUDE.md's actively-implementing cap
    for n, title in sorted(ready, key=lambda x: -x[0]):
        rid = f"issue-{n}"
        count = old_counts.get(rid, 0) + 1
        f.write(f"{rid} {n} {count} {title}\n")
os.replace(tmp, dq)
print(f"   OK: {len(ready)} ready, running={running}")
PY

# ---- [10] box evidence + closing-ref leak across every open agent/* PR -----
# check-box-evidence.sh is the per-PR CLI a human runs on one PR; wiring it
# in here by shelling out to it once per open PR would reintroduce the exact
# N-calls-over-ssh/API mistake this file's own header warns about (302 x
# 0.52s). box-evidence-bulk.py does the same two checks (box-check log vs
# HEAD, closing-ref leak) over data fetched in ONE extra bulk `gh pr list`
# call plus files this script already has lying around ($TMP/issues.json
# from the three-bulk-calls section, and a fresh worktree porcelain dump) --
# no per-PR gh/git call at all. A failed `git worktree list` writes the
# sentinel "WORKTREE-LIST-FAILED" rather than an empty file, so the python
# side can tell "the command failed" from "there happen to be zero linked
# worktrees" -- the same empty-result-vs-failed-query distinction as every
# other section in this file.
echo "-- [10] box evidence + closing-ref leak (open agent/* PRs, bulk)"
gh pr list --state open --limit 100 --json number,headRefOid,headRefName,closingIssuesReferences \
  > "$TMP/open_refs.json" 2>/dev/null || echo "   UNKNOWN: open-PR ref query failed"
git worktree list --porcelain > "$TMP/wt_porcelain.txt" 2>/dev/null \
  || echo "WORKTREE-LIST-FAILED" > "$TMP/wt_porcelain.txt"
if [ -s "$TMP/open_refs.json" ]; then
  /usr/bin/python3 "$SELFDIR/box-evidence-bulk.py" "$TMP/open_refs.json" "$TMP/issues.json" "$TMP/wt_porcelain.txt"
else
  echo "   UNKNOWN: open-PR ref query produced no data -- box-evidence check skipped"
fi

echo "== end $RUN =="
