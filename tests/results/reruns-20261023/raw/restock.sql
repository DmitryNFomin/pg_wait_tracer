\set s random(1, 901)
UPDATE stock s SET reserved = (SELECT count(*) FROM orders o WHERE o.item_id = s.item_id) WHERE s.item_id BETWEEN :s AND :s + 99;
