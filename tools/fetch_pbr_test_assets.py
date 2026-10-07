#!/usr/bin/env python3
"""Fetch pinned PBR assets, verify SHA-256, and prepare labelled test variants."""
import argparse
import copy
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import tempfile
from urllib.request import urlopen


PROJECT = Path(__file__).resolve().parents[1]
MODELS = PROJECT / 'assets/models'
MANIFEST = Path(__file__).with_name('pbr_test_assets.json')


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            checksum.update(data)
    return checksum.hexdigest()


def acquire(item, verify_only):
    dest = MODELS / item['path']
    if not dest.resolve().is_relative_to(MODELS.resolve()):
        raise ValueError(f"Asset escapes models directory: {item['path']}")
    if (dest.is_file() and dest.stat().st_size == item['bytes']
            and digest(dest) == item['sha256']):
        return
    if verify_only:
        raise ValueError(f"Missing or damaged asset: {item['path']}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=dest.name + '.', suffix='.part',
                                dir=dest.parent)
    part = Path(name)
    try:
        with os.fdopen(fd, 'wb') as out, urlopen(item['url'], timeout=30) as source:
            while data := source.read(1024 * 1024):
                out.write(data)
        if part.stat().st_size != item['bytes'] or digest(part) != item['sha256']:
            raise ValueError(f"Asset checksum mismatch: {item['path']}")
        part.replace(dest)
        print(f"Downloaded {item['path']}", flush=True)
    finally:
        part.unlink(missing_ok=True)


def cutaway():
    source = MODELS / 'pbr_kitchen/source/kitchen_core.gltf'
    scene = json.loads(source.read_text())
    # Keep every authored material/transform. This inspection variant opens the
    # room for the current viewer's bounds-fit camera; the original stays intact.
    removed = {126: '126_Ceiling', 127: '127_Walls',
               263: '263_Walls', 264: '264_Walls'}
    for index, name in removed.items():
        if scene['nodes'][index]['name'] != name:
            raise ValueError('Pinned kitchen node layout changed')
    root = scene['nodes'][300]
    if root['name'] != 'kitchen_root':
        raise ValueError('Pinned kitchen root changed')
    root['children'] = [n for n in root['children'] if n not in removed]
    scene['buffers'][0]['uri'] = 'source/kitchen.bin'
    scene.setdefault('extras', {})['projectTestVariant'] = {
        'name': 'kitchen_cutaway',
        'originalAuthor': 'Jay-Artist',
        'license': 'https://creativecommons.org/licenses/by/3.0/',
        'modification': 'Remove ceiling/wall nodes from the active scene only',
        'removedNodes': list(removed.values()),
        'purpose': 'Material inspection with bounds-fit camera; not enclosed-room lighting reference',
    }
    dest = MODELS / 'pbr_kitchen/kitchen_cutaway.gltf'
    dest.write_text(json.dumps(scene, indent=2) + '\n')
    return dest


def material_variants():
    """Keep upstream payloads intact; distinguish volume tint from surface tint."""
    folder = MODELS / 'pbr_green_glass_dragon'
    scene = json.loads((folder / 'source/DragonAttenuation.gltf').read_text())
    if (scene['materials'][1]['name'] != 'Dragon with Attenuation'
            or scene['nodes'][1]['name'] != 'Dragon'):
        raise ValueError('Pinned dragon material/node layout changed')
    for resource in scene.get('buffers', []) + scene.get('images', []):
        if 'uri' in resource and not resource['uri'].startswith('data:'):
            resource['uri'] = 'source/' + resource['uri']
    green = [0.12, 0.85, 0.25]
    material = scene['materials'][1]
    material['name'] = 'Green glass - volume absorption'
    # A glass absorption colour belongs to the volume, not diffuse albedo or
    # alpha. Keep full transmission, thickness, roughness and authored scale.
    material['extensions']['KHR_materials_volume']['attenuationColor'] = green
    scene.setdefault('extras', {})['projectTestVariant'] = {
        'name': 'green_glass_dragon',
        'source': 'Khronos DragonAttenuation / Stanford dragon',
        'modification': 'Change active dragon volume attenuationColor only; rebase resource URIs',
        'requires': ['KHR_materials_transmission', 'KHR_materials_volume'],
        'expectedCurrentRenderer': 'Unsupported extensions; opaque white core fallback, not green glass',
    }
    glass = folder / 'green_glass_dragon.gltf'
    glass.write_text(json.dumps(scene, indent=2) + '\n')

    control = copy.deepcopy(scene)
    for material in control['materials']:
        material.pop('extensions', None)
    control['materials'][1]['name'] = 'Green OPAQUE surface - diagnostic control'
    control['materials'][1]['pbrMetallicRoughness']['baseColorFactor'] = green + [1]
    control.pop('extensions', None)
    control.pop('extensionsUsed', None)
    control.pop('extensionsRequired', None)
    for mesh in control['meshes']:
        for primitive in mesh['primitives']:
            primitive.pop('extensions', None)
    control['extras']['projectTestVariant'] = {
        'name': 'green_surface_control',
        'source': 'Khronos DragonAttenuation / Stanford dragon',
        'modification': 'Remove material/variant extensions; tint core opaque surface green',
        'purpose': 'Surface-colour control only; NOT glass or jade',
    }
    surface = folder / 'green_surface_control.gltf'
    surface.write_text(json.dumps(control, indent=2) + '\n')

    folder = MODELS / 'pbr_anisotropy_barn_lamp'
    lamp = json.loads((folder / 'source/AnisotropyBarnLamp.gltf').read_text())
    changed = 0
    for material in lamp['materials']:
        anisotropy = material.get('extensions', {}).get('KHR_materials_anisotropy')
        if anisotropy is not None:
            anisotropy['anisotropyStrength'] = 0
            changed += 1
    if changed != 1:
        raise ValueError('Pinned lamp anisotropy material layout changed')
    for resource in lamp.get('buffers', []) + lamp.get('images', []):
        if 'uri' in resource and not resource['uri'].startswith('data:'):
            resource['uri'] = 'source/' + resource['uri']
    lamp.setdefault('extras', {})['projectTestVariant'] = {
        'name': 'anisotropy_off_control',
        'modification': 'Set anisotropyStrength to zero only; rebase resource URIs',
        'purpose': 'Paired high-light direction check; current unsupported renderer should be unchanged',
    }
    isotropic = folder / 'anisotropy_off_control.gltf'
    isotropic.write_text(json.dumps(lamp, indent=2) + '\n')
    return [glass, surface, isotropic]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', action='store_true',
                        help='Verify local files without accessing the network')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda item: acquire(item, args.verify_only), manifest['files']))
    variants = [cutaway(), *material_variants()]
    total = sum(item['bytes'] for item in manifest['files'])
    print(f"Verified {len(manifest['files'])} files, {total:,} bytes.")
    for variant in variants:
        print(f"Test variant: {variant}")


if __name__ == '__main__':
    main()
