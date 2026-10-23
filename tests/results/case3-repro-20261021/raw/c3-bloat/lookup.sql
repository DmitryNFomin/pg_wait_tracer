\set id random(1, 200000)
SELECT filler FROM t WHERE id = :id;
