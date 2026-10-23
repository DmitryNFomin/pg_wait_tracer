SELECT bid, count(*), sum(abalance), avg(abalance) FROM pgbench_accounts GROUP BY bid;
