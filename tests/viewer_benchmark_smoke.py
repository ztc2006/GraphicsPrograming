#!/usr/bin/env python3
"""Run manually in a graphical Vulkan session; validate real report readback."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, scene = (str(Path(p).resolve()) for p in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='vulkan-benchmark-smoke-') as temporary:
    result = subprocess.run([binary, '--benchmark', temporary, '--warmup', '0', '--duration', '1', '--size', '800x600', '--no-ui', scene], capture_output=True, text=True, timeout=45)
    print(result.stdout + result.stderr)
    assert result.returncode == 0, f'Viewer shutdown/report failed: {result.returncode}'
    summary = json.loads((Path(temporary)/'summary.json').read_text())
    rows = list(csv.DictReader((Path(temporary)/'frames.csv').open()))
    assert summary['schema'] == 3
    assert summary['completed'] and len(rows) > 0
    assert len(rows) == summary['frame_count']
    assert len({row['frame_id'] for row in rows}) == len(rows)
    assert summary['gpu_sample_count'] == len(rows), 'Completed frames must have GPU queries'
    for row in rows:
        assert None not in row, 'CSV schema mismatch'
        assert row['gpu_valid'] == '1'
        total = float(row['gpu_total_ms'])
        assert total > 0 and total >= float(row['gpu_main_ms'])
        assert total >= float(row['gpu_shadow_ms'])
        assert 0 < float(row['gpu_output_ms']) <= total
    ledger = summary['engine_owned_resources']
    assert ledger['current']['allocated_bytes'] > 0
    assert ledger['peak']['allocated_bytes'] >= ledger['current']['allocated_bytes']
    for field in ledger['current']:
        assert sum(domain[field] for domain in ledger['domains'].values()) == ledger['current'][field], field
    assert ledger['domains']['staging']['suballocated_bytes'] == 0
    assert ledger['domains']['prepared_scene']['suballocated_bytes'] == 0
    assert ledger['domains']['retired_scene']['suballocated_bytes'] == 0
    assert ledger['domains']['live_scene']['buffers'] > 0
    assert ledger['domains']['live_scene']['suballocated_bytes'] > 0
    assert ledger['current']['suballocations'] == ledger['current']['buffers'] + ledger['current']['images']
    assert ledger['domains']['shared_textures']['suballocated_bytes'] > 0
    for owner, domain in ledger['domains'].items():
        if owner != 'allocator_blocks':
            assert domain['allocated_bytes'] == 0, owner
    assert ledger['domains']['allocator_blocks']['allocated_bytes'] > 0
    assert ledger['domains']['allocator_blocks']['suballocated_bytes'] == 0
    if summary['software_device']:
        assert not summary['hardware_target_accepted']
    print(f"PASS: {len(rows)} frames, matching GPU queries, clean shutdown and valid report")
