import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { runInNewContext } from 'node:vm';
import { buildTableModel } from '../../web/static/lib/table.js';
import { overviewConfig } from '../../web/static/lib/builders/table-configs.js';

// Read the driver's actual predicate with Python's parser, without importing
// its unrelated browser/image dependencies. Missing or non-literal predicates
// fail this suite; there is no fallback or copy of the render predicate.
const predicate = JSON.parse(execFileSync('python3', ['-c', `
import ast, json, pathlib, sys
tree = ast.parse(pathlib.Path(sys.argv[1]).read_text())
checks = next(node.value for node in tree.body
              if isinstance(node, ast.Assign) and any(
                  isinstance(target, ast.Name) and target.id == 'PANEL_CHECKS'
                  for target in node.targets))
overview = next(value for key, value in zip(checks.keys, checks.values)
                if ast.literal_eval(key) == 'overview')
print(json.dumps(ast.literal_eval(overview)))
`, fileURLToPath(new URL('../ui_live_smoke.py', import.meta.url))], {
    encoding: 'utf8',
}));

for (const [ms, expected] of [[0, false], [0.001, true], [10, true], [null, false]]) {
    test(`Overview one-row response ms=${ms}: ok=${expected}`, () => {
        // Use the real builder/formatter to render the server's one-row
        // response, then evaluate the driver's actual render predicate.
        // No replica of duration formatting or render predicate.
        const response = { rows: [{ name: 'DB Time', ms, indent: 0 }] };
        const model = buildTableModel(overviewConfig, response.rows, null);
        const rows = model.rows.map(row => ({
            cells: row.cells.map(cell => ({ textContent: cell.html })),
        }));
        const check = runInNewContext('(' + predicate + ')', {
            document: { querySelectorAll: () => rows },
        });
        const result = check();
        console.log('Overview one-row response', JSON.stringify({ response, result }));
        assert.equal(result.ok, expected, JSON.stringify(result));
        if (ms === 0) {
            assert.equal(result.detail,
                'DB Time <1µs does not establish DB Time > 0');
        }
    });
}
