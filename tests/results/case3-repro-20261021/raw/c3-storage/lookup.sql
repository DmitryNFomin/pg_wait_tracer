\set id random(1, 12000000)
SELECT filler FROM t WHERE id = :id;
