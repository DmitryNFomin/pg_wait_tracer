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
    out=$(ssh -o BatchMode=yes -o ConnectTimeout=4 -o StrictHostKeyChecking=no "$box" \
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

echo "== end $RUN =="
