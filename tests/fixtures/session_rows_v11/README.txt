Issue #852 legacy accounting fixture

current.summary and current.summary.meta were generated on origin/master
4c2f6b87e239679679a2d725087401bb819bf68f, before applying the writer fix,
using tests/gen_test_traces and test_data_sessions.row_scenario().
The summary header has version 11 and an identity mono-to-wall mapping.
It contains the original pid-only idle rows for pids 9000 and 9001.

To reproduce on that production revision with the new test helper available:
  python3 - <<'PY'
  import sys
  sys.path.insert(0, 'tests')
  from server_harness import generate_traces
  from test_data_sessions import row_scenario
  print(generate_traces(row_scenario()))
  PY
Copy current.summary and current.summary.meta from the printed directory.

test_data_sessions.py generates the matching raw capture afresh, substitutes
these frozen summary files, and requires accounting-version refusal plus the
same session membership/order as the raw capture. Keeping the old bytes is
essential: restamping a newly generated summary would omit the defective
rows and could not test what happens to existing files.
