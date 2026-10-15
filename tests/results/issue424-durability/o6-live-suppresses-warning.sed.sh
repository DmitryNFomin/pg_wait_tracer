# Mutant: UNKNOWN/IN_PROGRESS stop warning even when nobody is writing — the
# copied-capture case goes silent.
d=$1
perl -0pi -e 's/    case PGWT_CLOSURE_IN_PROGRESS:\n    case PGWT_CLOSURE_UNKNOWN:/    case PGWT_CLOSURE_IN_PROGRESS:\n    case PGWT_CLOSURE_UNKNOWN:\n        return false;   \/* mutant: never warn *\//' $d/trace_closure.h
grep -q 'mutant: never warn' $d/trace_closure.h
