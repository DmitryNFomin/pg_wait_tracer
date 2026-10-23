\set x random(1, 980000)
BEGIN;
UPDATE pgbench_accounts SET abalance = abalance WHERE aid BETWEEN :x AND :x + 20000;
COMMIT;
