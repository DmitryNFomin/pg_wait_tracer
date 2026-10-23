# client.py OUT SECONDS RANGE TIMEOUT_MS [WORKERS]: PK lookups with statement_timeout; keeps going after cancellations.
# Logs per request: start_epoch end_epoch backend_pid id status(ok|timeout|err)
import sys, time, random, multiprocessing as mp, psycopg
out, secs, rng, tmo = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
W = int(sys.argv[5]) if len(sys.argv) > 5 else 8
def worker(i, deadline):
    c = psycopg.connect("host=/var/run/postgresql port=5418 user=postgres dbname=c3 application_name=app", autocommit=True)
    c.execute(f"set statement_timeout = {tmo}")
    pid = c.info.backend_pid
    r = random.Random(i * 7919 + int(time.time()))
    with open(f"{out}.{i}", "w") as f:
        while time.time() < deadline:
            k = r.randint(1, rng); t0 = time.time(); st = "ok"
            try:
                c.execute("SELECT filler FROM t WHERE id = %s", (k,)).fetchall()
            except psycopg.errors.QueryCanceled:
                st = "timeout"
            except Exception as e:
                st = "err"
            f.write(f"{t0:.6f} {time.time():.6f} {pid} {k} {st}\n")
if __name__ == "__main__":
    dl = time.time() + secs
    ps = [mp.Process(target=worker, args=(i, dl)) for i in range(W)]
    [p.start() for p in ps]; [p.join() for p in ps]
