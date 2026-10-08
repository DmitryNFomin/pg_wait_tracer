-- b_ior: IO:DataFileRead. An indexed single-row read at a random id over a
-- table provisioned well above shared_buffers (tests/provision-runner.sh
-- builds _smoke_io_load at 3,000,000 rows x ~200B of filler), so most
-- lookups miss the buffer cache and do a real relation read.
\set id random(1, 3000000)
SELECT filler FROM _smoke_io_load WHERE id = :id;
