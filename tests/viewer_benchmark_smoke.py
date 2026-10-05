#!/usr/bin/env python3
"""Run manually in a graphical Vulkan session; validate real report readback."""
import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, scene = (str(Path(p).resolve()) for p in sys.argv[1:3])
frames = sys.argv[3] if len(sys.argv) > 3 else "1"
sync = sys.argv[4] if len(sys.argv) > 4 else "auto"
with tempfile.TemporaryDirectory(prefix='vulkan-benchmark-smoke-') as temporary:
    result = subprocess.run([binary, '--benchmark', temporary, '--warmup', '0', '--duration', '1', '--size', '800x600', '--no-ui', '--frames-in-flight', frames, '--present-sync', sync, scene], capture_output=True, text=True, timeout=45)
    print(result.stdout + result.stderr)
    assert result.returncode == 0, f'Viewer shutdown/report failed: {result.returncode}'
    summary = json.loads((Path(temporary)/'summary.json').read_text())
    rows = list(csv.DictReader((Path(temporary)/'frames.csv').open()))
    assert summary['schema'] == 4
    assert summary['frames_in_flight'] == int(frames)
    assert summary['swapchain_image_count'] >= 2
    assert summary['frame_target_policy'] == 'shared_hdr_depth_shadow'
    assert summary['completed'] and len(rows) > 0
    assert summary['pending_present_fences'] == 0
    assert summary['present_queued_count'] == len(rows)
    if summary['present_fences_enabled']:
        assert summary['presentation_release_proven']
        assert summary['present_fence_completed_count'] == len(rows)
        assert summary['present_sync_backend'] in ('KHR_present_fence', 'EXT_present_fence')
    else:
        assert not summary['presentation_release_proven']
        assert summary['present_sync_backend'] == 'legacy_wait_idle'
        assert summary['legacy_present_drain_count'] > 0
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
