#!/usr/bin/env python3
"""VOID fuzz and refusal diagnostics for the U4/U5 reviewer scripts.

CFLAGS is passed through to the portable C build. Numerical/projection/diagnostic
mismatches and answering an ambiguous reference input fail the check. Conservative
whole-input refusals are counted separately with their public replay reason.
"""
import argparse
from collections import Counter
import contextlib
import io
import json
from pathlib import Path
import runpy
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
import test_core_evidence_differential as d
import evidence_void_inputs as void
import evidence_file_inputs as files


def check(binary, work, cases, label):
    ref=d.reference_module()
    answers=[]
    for start in range(0,len(cases),250):
        paths=[]
        for i,c in enumerate(cases[start:start+250],start):
            p=work/f"fuzz-{i}.json"
            p.write_text(json.dumps(c))
            paths.append(p)
        result=d.execute([binary,"--json-reasons"]+paths)
        if result.returncode or d.sanitizer_error(result.stdout+result.stderr):
            raise RuntimeError(f"fuzz driver failed: {result.stdout}\n{result.stderr}")
        lines=[json.loads(line) for line in result.stdout.splitlines()]
        if len(lines)!=len(paths): raise RuntimeError("fuzz driver output count")
        answers.extend(lines)
    values=0
    unsafe=0
    refusals=Counter()
    ambiguities=Counter()
    for c,actual in zip(cases,answers):
        try:
            ref.section_7_scenario(c)
        except ref.Ambiguity as error:
            ambiguities[str(error)]+=1
        want=d.reference_answer(ref,c)
        reason=actual.pop("reason",None)
        if actual==want: continue
        if "error" in actual and "error" not in want:
            refusals[reason or "unspecified"]+=1
        elif "error" in want:
            unsafe+=1
            print(f"{label} C answered ambiguity: {c['id']} {json.dumps(c)}")
        else:
            values+=1
            print(f"{label} value difference: {c['id']} {d.first_difference(actual,want)} {json.dumps(c)}")
    print(f"{label}: {len(cases)} cases; value differences={values}; "
          f"refusal-only differences={sum(refusals.values())}; C answers ambiguity={unsafe}; "
          f"reference ambiguities={sum(ambiguities.values())}")
    for reason,n in sorted(refusals.items()): print(f"  refusal-only {n}: {reason}")
    for reason,n in sorted(ambiguities.items()): print(f"  reference refusal {n}: {reason}")
    return bool(values or unsafe)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases",type=int,default=4000)
    parser.add_argument("--seed",type=int,default=1)
    scripts=parser.add_mutually_exclusive_group()
    scripts.add_argument("--u4-script",type=Path)
    scripts.add_argument("--u5-script",type=Path)
    parser.add_argument("--file-chains",action="store_true")
    args=parser.parse_args()
    cases=None
    script=args.u4_script or args.u5_script
    if script:
        # Execute the exact reviewer script, then inspect its same corpus using
        # the public refusal reason. Clean its unscoped mkdtemp on completion.
        sys.argv=[str(script),str(d.ROOT),str(args.cases),str(args.seed)]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            g=runpy.run_path(str(script))
        print(output.getvalue(),end="")
        cases=g["cases"]
        shutil.rmtree(g["work"])
    else:
        cases=void.generate(d.fixture("01"),args.cases,args.seed)
    with tempfile.TemporaryDirectory(prefix="pgwt-evidence-fuzz-") as directory:
        work=Path(directory)
        binary=work/"replay"
        d.build(d.ROOT,binary)
        label=("U4" if args.u4_script else "U5" if args.u5_script else "VOID")+f" fuzz seed={args.seed}"
        failed=check(binary,work,cases,label)
        if args.file_chains:
            failed |= check(binary,work,files.extend(cases,args.seed),label+" file chains")
        return failed


if __name__=="__main__":
    try:
        sys.exit(main())
    except (OSError,ValueError,RuntimeError) as error:
        print(f"evidence fuzz harness error: {error}",file=sys.stderr)
        sys.exit(2)
