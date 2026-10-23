\set a random(1, 998000)
\set b random(1, 1000000)
BEGIN;
SELECT sum(abalance) FROM pgbench_accounts WHERE aid BETWEEN :a AND :a + 2000;
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid = :b;
COMMIT;
