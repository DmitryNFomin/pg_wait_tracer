/* pgwt — Node-side import path for the deterministic gallery fixtures (U1).
 * Canonical data lives in web/static/dev/fixtures/ because the gallery page
 * can only fetch below the static root (web/static/) that tests/mock_server.py
 * serves; this shim gives Node tests the stable tests/fixtures/* path. */
export * from '../../web/static/dev/fixtures/table-configs.mjs';
