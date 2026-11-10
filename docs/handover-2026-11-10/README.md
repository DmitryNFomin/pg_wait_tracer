# Local-only artefacts, committed at the 2026-11-10 handover

Everything in this directory existed on one Mac and nowhere else when the owner
stopped new implementation. It is committed so that nothing of value dies with a
session scratchpad or a `/private/tmp` cleanup. **Read every file here as dated
material, not as current truth**: the repository, the issues and the measurements
have moved since each was written, and some of them are superseded in ways the
files themselves do not know about.

| file | written | what it is | how much to trust it |
|---|---|---|---|
| `TAG_PLAN_2026-11-09.md` | 2026-11-09 | the label-change proposal the owner has not approved: close #771 and #381, drop the label from #832, #760, #622 and #573, mark #341 a duplicate of #808, **add** the label to #306, and treat #757 as a judgement call | live and actively cited; `docs/HANDOVER_2026-11-10.md` section 4 depends on it. Two of its rationales are stale: it proposes closing #381 as "superseded by #818", and #818 has since closed while #381's divergence has been measured and traced to window-boundary placement |
| `RELEASE-PLAN.md` | 2026-10-16 | the plan to a tagged demo as it stood three weeks before this handover | historical. Its issue list predates the current twenty open blockers |
| `SEMANTIC-CONTRACTS.md` | 2026-10-16 | what each displayed quantity is supposed to mean | the most reusable of the historical set, because the contracts change more slowly than the issue list. Still unverified against today's code |
| `HANDOVER-424.md` | 2026-10-16 | a 60 KB handover for the owner's final check on #424 and the issues around it | historical, and the largest single piece of analysis in this directory |
| `HANDOVER-REVIEW-LEAD.md` | 2026-10-22 | the lead's position on an earlier handover document | historical |
| `MERGE-ORDER-POST-TAG.md` | 2026-10-23 | the intended merge order for the parked branches after a tag, against candidate `8ab558b` | relevant again only if PRs #33 and #34 are unparked; the owner parked them on 2026-11-02 |
| `MORNING-STATUS.md` | 2026-10-16 | a day's status on nine defects | historical |
| `PRESENTER-NOTES-8ab558b.md` | 2026-10-22 | presenter notes for a demo candidate | historical |
| `stash-reviewer-ui-mac-walk-20260928.patch` | 2026-09-28 | a reviewer's `git stash` entry, found during the handover cleanup: 72 added lines across `tests/ui_mac_walk.sh`, `tests/ui_mac_walk_lib.py`, `tests/ui_mac_walk_safari.py` and `tests/test_ui_mac_walk_lib.py` | **do not apply.** None of those four files exists on master any more; they were tracked once and have since been removed. It is kept only so the work is not silently dropped, and the stash itself was deleted after this copy was made |

| `EVIDENCE_MANIFEST.md` | 2026-11-10 | every result-bearing file in the moved capture directories, with size and SHA-256 prefix | live; it is how a found copy is identified and how a loss is proven |
| `REPOSITORY_TRAPS.md` | 2026-11-10 | the repository-specific subset of the outgoing lead's working memory | unverified working notes, never audited; the code wins wherever they disagree |
| `issue115-harness/` | 2026-09-27, copied 2026-11-10 | the #194 verification harness: the kernel-decomposition evidence (`EVIDENCE.txt`, `EVIDENCE_BIAS.txt`) and the capture/analysis scripts that produced it, 180 KB of text | the measurements are October's and were not re-run this week; the files are a byte-identical copy of two directories that were untracked and existed only on the outgoing lead's Mac. See its own `README.md`, which also names the four files that are **not** here and the pushed `preserve/*` refs that hold three of them |

The measurement evidence behind the numbers published on #381, #837 and #839 is
**not** here: those captures are about 1.1 GB and were moved, as the last act of
the handover, to `tests/results/handover-2026-11-10-evidence/` -- untracked, but
durable on disk rather than in `/private/tmp`. `docs/HANDOVER_2026-11-10.md` section 7 lists their
paths and says how to re-derive a published number from them. If that machine is
wiped, the published numbers remain in the issue comments and the raw data does not.
