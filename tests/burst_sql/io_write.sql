-- b_iow: IO:DataFileWrite. Twenty scattered single-row UPDATEs per
-- transaction dirty twenty widely separated pages; the backend itself ends
-- up writing some of them back when it needs a clean buffer, which is
-- exactly the dirty-buffer eviction write a real write-heavy system shows.
UPDATE _smoke_io_load SET ctr = ctr + 1 WHERE id IN (SELECT (random() * 2999999)::int + 1 FROM generate_series(1, 20));
