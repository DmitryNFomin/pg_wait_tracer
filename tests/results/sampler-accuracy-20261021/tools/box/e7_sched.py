import sys
for tag in sys.argv[1:]:
    s = {l.split()[0]: list(map(int, l.split()[1:3])) for l in open(f"sched-start-{tag}.txt") if len(l.split()) >= 3}
    e = {l.split()[0]: list(map(int, l.split()[1:3])) for l in open(f"sched-end-{tag}.txt") if len(l.split()) >= 3}
    on = sum(e[p][0]-s[p][0] for p in s if p in e)/1e9; rq = sum(e[p][1]-s[p][1] for p in s if p in e)/1e9
    n = len([p for p in s if p in e])
    print(f"{tag}: victims={n} over 56 s: on-CPU {on:.1f} s, run-queue {rq:.1f} s ({100*rq/(56*n):.1f}% of victim wall)")
