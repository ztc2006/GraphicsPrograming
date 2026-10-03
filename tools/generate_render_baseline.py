#!/usr/bin/env python3
"""Generate deterministic glTF material/alpha/transform fixtures (stdlib only)."""
import base64
import json
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1] / "assets/render_tests"


def make_png(path):
    rows = bytearray()
    for y in range(64):
        rows.append(0)
        for x in range(64):
            c = 235 if (x // 4 + y // 4) % 2 else 35
            a = 255 if (x - 32)**2 + (y - 32)**2 < 28**2 else 0
            rows.extend((c, c, c, a))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 64, 64, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def generate():
    ROOT.mkdir(parents=True, exist_ok=True)
    make_png(ROOT / 'coverage.png')
    blob = bytearray()
    doc = {'asset': {'version': '2.0', 'generator': 'GraphicsPrograming M0 baseline v1'}, 'buffers': [], 'bufferViews': [], 'accessors': [], 'meshes': [], 'nodes': [], 'materials': []}
    def accessor(data, kind, width, component=5126, target=34962):
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(struct.pack('<' + ('f' if component == 5126 else 'I') * len(data), *data))
        view = len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(blob)-offset, 'target': target})
        result = {'bufferView': view, 'componentType': component, 'count': len(data)//width, 'type': kind}
        if kind == 'VEC3':
            result['min'] = [min(data[i::width]) for i in range(width)]
            result['max'] = [max(data[i::width]) for i in range(width)]
        doc['accessors'].append(result)
        return len(doc['accessors'])-1
    positions, normals, uv, indices = [], [], [], []
    lat, lon = 24, 48
    for i in range(lat+1):
        theta = math.pi*i/lat
        for j in range(lon+1):
            phi = 2*math.pi*j/lon
            n = [math.sin(theta)*math.cos(phi), math.cos(theta), math.sin(theta)*math.sin(phi)]
            normals.extend(n); positions.extend(.43*x for x in n); uv.extend((j/lon, i/lat))
    for i in range(lat):
        for j in range(lon):
            a = i*(lon+1)+j; b = a+lon+1
            if i != 0: indices.extend((a,a+1,b))
            if i != lat-1: indices.extend((a+1,b+1,b))
    sphere = {'POSITION': accessor(positions,'VEC3',3), 'NORMAL': accessor(normals,'VEC3',3), 'TEXCOORD_0': accessor(uv,'VEC2',2)}
    sphere_indices = accessor(indices,'SCALAR',1,5125,34963)
    for row in range(2):
        for column, roughness in enumerate((.05,.2,.4,.7,1.0)):
            name = f"{'Metal' if row else 'Dielectric'} roughness {roughness}"
            mat = len(doc['materials']); mesh = len(doc['meshes'])
            doc['materials'].append({'name':name,'pbrMetallicRoughness':{'baseColorFactor':[.8,.52,.19,1], 'metallicFactor':row, 'roughnessFactor':roughness}})
            doc['meshes'].append({'name':name,'primitives':[{'attributes':sphere,'indices':sphere_indices,'material':mat}]})
            doc['nodes'].append({'name':name,'mesh':mesh,'translation':[column-2,1.0+row,0]})
    plane = {'POSITION':accessor([-3,0,-2,-3,0,2,3,0,2,3,0,-2],'VEC3',3), 'NORMAL':accessor([0,1,0]*4,'VEC3',3),'TEXCOORD_0':accessor([0,0,0,24,24,24,24,0],'VEC2',2)}
    quad_indices = accessor([0,1,2,0,2,3],'SCALAR',1,5125,34963)
    doc['images'] = [{'uri':'coverage.png'}]
    doc['samplers'] = [{'magFilter':9728,'minFilter':9987,'wrapS':10497,'wrapT':10497}]
    doc['textures'] = [{'source':0,'sampler':0}]
    for name, alpha, translation, scale in [('Floor/UV repetition','OPAQUE',[0,0,0],[1,1,1]), ('Alpha mask card','MASK',[-2.2,.4,1.4],[.18,1,.18]), ('Alpha blend card','BLEND',[2.2,.4,1.4],[.18,1,.18]), ('Mirrored single-sided card','OPAQUE',[0,.4,1.4],[-.18,1,.18])]:
        mat = len(doc['materials']); mesh = len(doc['meshes'])
        doc['materials'].append({'name':name,'alphaMode':alpha,'alphaCutoff':.5,'doubleSided':False,'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'baseColorFactor':[1,1,1,.5 if alpha=='BLEND' else 1],'metallicFactor':0,'roughnessFactor':.8}})
        doc['meshes'].append({'name':name,'primitives':[{'attributes':plane,'indices':quad_indices,'material':mat}]})
        doc['nodes'].append({'name':name,'mesh':mesh,'translation':translation,'scale':scale})
    doc['buffers'] = [{'byteLength':len(blob),'uri':'data:application/octet-stream;base64,'+base64.b64encode(blob).decode()}]
    doc['scenes'] = [{'name':'M0 material baseline','nodes':list(range(len(doc['nodes'])))}]; doc['scene'] = 0
    (ROOT/'material_baseline.gltf').write_text(json.dumps(doc,indent=2)+'\n')
    print(ROOT/'material_baseline.gltf')


if __name__ == '__main__':
    generate()
