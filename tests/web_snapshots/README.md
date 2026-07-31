# Visual-regression baselines (Phase B4)

The `*.png` files in this directory are the committed baseline screenshots for
`tests/test_web_ui_snapshots.py`. The CI `snapshots` job screenshots each web
view against the fixed `mock_server.py` dataset and diffs against these.

## Do NOT regenerate baselines locally

Playwright screenshots are environment-sensitive (font rendering, antialiasing,
chromium build). Baselines must be produced in the **same** environment that
compares them — `ubuntu-latest` + the chromium build bundled with the
**pinned** playwright version. Note that `playwright install` pins the browser
per playwright *version* only (each playwright release bundles one specific
chromium build; the command itself follows whatever playwright is installed),
so playwright must be — and is — pinned in `.github/workflows/ci.yml`, with
the exact playwright + chromium versions recorded in `VERSION` next to this
file. Local rendering will not match and will churn diffs.

## Regenerating baselines (intentional UI change or playwright bump)

When a UI change legitimately alters appearance, regenerate the baselines in CI:

```
gh workflow run CI --ref <your-branch> -f update_snapshots=true
gh run watch                              # wait for the snapshots job
gh run download <run-id> -n snapshot-baselines -D tests/web_snapshots/
git add tests/web_snapshots/*.png
git commit -m "B4: update visual-snapshot baselines (<why>)"
```

Then the normal `snapshots` job (compare mode) on the PR validates them.

When bumping playwright: update BOTH pins in `.github/workflows/ci.yml` (the
web-ui and snapshots jobs), regenerate the baselines as above, and update
`VERSION` — all in the same PR.

## Transient artifacts

On a compare failure the suite writes `<name>-actual.png` and `<name>-diff.png`
next to the baseline (uploaded by CI as the `snapshot-diffs` artifact). These
are git-ignored — only the baselines are tracked.
