\set c random(1, 3000000)
\set item random(1, 1000)
BEGIN;
SELECT name, descr FROM catalog WHERE id = :c;
UPDATE stock SET qty = qty - 1 WHERE item_id = :item;
COMMIT;
