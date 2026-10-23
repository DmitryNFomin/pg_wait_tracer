\set id random(1, 10000000)
SELECT payload FROM t WHERE id = :id;
