"""Read-only inspection of compressed Fable UE3 mesh packages; no game process."""
from pathlib import Path
import struct
import json
import math
import sys
import hashlib

ROOT = Path(__file__).resolve().parents[3]  # workspace containing src/ and test-install/


def u32(data, off):
    return struct.unpack_from('<I', data, off)[0]


def lzo_block(data, expected):
    """Decode the classic LZO1X instruction stream with strict output bounds."""
    ip = 0
    out = bytearray()
    state = 0

    def byte():
        nonlocal ip
        if ip >= len(data):
            raise ValueError('truncated LZO instruction')
        value = data[ip]
        ip += 1
        return value

    def length(value, mask):
        if value:
            return value
        result = mask
        while True:
            extra = byte()
            if extra:
                return result + extra
            result += 255

    def literal(count):
        nonlocal ip
        if count > len(data) - ip or len(out) + count > expected:
            raise ValueError('invalid LZO literal length')
        out.extend(data[ip:ip + count])
        ip += count

    if data[0] > 17:
        count = byte() - 17
        literal(count)
        state = min(count, 4)
    while True:
        op = byte()
        if op < 16 and state == 0:
            literal(length(op, 15) + 3)
            state = 4
            continue
        if op < 16:
            distance = 1 + (op >> 2) + (byte() << 2)
            count = 2
            if state == 4:
                distance += 2048
                count = 3
            tail = op & 3
        elif op < 32:
            count = length(op & 7, 7) + 2
            operand = byte() | (byte() << 8)
            distance = 16384 + ((op & 8) << 11) + (operand >> 2)
            if distance == 16384:
                if count != 3 or len(out) != expected or ip != len(data):
                    raise ValueError('invalid LZO end marker/size')
                return bytes(out)
            tail = operand & 3
        elif op < 64:
            count = length(op & 31, 31) + 2
            operand = byte() | (byte() << 8)
            distance = 1 + (operand >> 2)
            tail = operand & 3
        else:
            count = (op >> 5) + 1
            distance = 1 + ((op >> 2) & 7) + (byte() << 3)
            tail = op & 3
        if distance > len(out) or len(out) + count > expected:
            raise ValueError('invalid LZO dictionary reference')
        for _ in range(count):
            out.append(out[-distance])
        literal(tail)
        state = tail


def package_bytes(path):
    packed = path.read_bytes()
    if u32(packed, 0) != 0x9E2A83C1:
        raise ValueError('not a UE3 package')
    # Fable Anniversary 860/26985 summary with group name "None".
    if struct.unpack_from('<HH', packed, 4) != (860, 26985):
        raise ValueError('unsupported package version')
    if packed[12:21] != b'\x05\0\0\0None\0' or u32(packed, 0x55) != 1:
        raise ValueError('unsupported summary layout')
    flags, count = struct.unpack_from('<II', packed, 0x6D)
    if flags != 2 or not 0 < count < 1024:
        raise ValueError('expected chunked LZO package')
    chunks = [struct.unpack_from('<4I', packed, 0x75 + 16 * i) for i in range(count)]
    total = max(start + size for start, size, _, _ in chunks)
    plain = bytearray(total)
    plain[:chunks[0][0]] = packed[:chunks[0][0]]
    for start, size, offset, compressed_size in chunks:
        tag, block_size, payload_size, output_size = struct.unpack_from('<4I', packed, offset)
        if tag != 0x9E2A83C1 or output_size != size:
            raise ValueError('invalid compressed chunk')
        blocks = (size + block_size - 1) // block_size
        entries = [struct.unpack_from('<II', packed, offset + 16 + 8 * i) for i in range(blocks)]
        cursor = offset + 16 + 8 * blocks
        written = 0
        for compressed, uncompressed in entries:
            block = packed[cursor:cursor + compressed]
            decoded = block if compressed == uncompressed else lzo_block(block, uncompressed)
            plain[start + written:start + written + uncompressed] = decoded
            written += uncompressed
            cursor += compressed
        if written != size or cursor != offset + compressed_size or sum(c for c, _ in entries) != payload_size:
            raise ValueError('chunk bounds/size mismatch')
    return plain


def name_table(data):
    count, offset = struct.unpack_from('<II', data, 25)
    names = []
    for _ in range(count):
        size = struct.unpack_from('<i', data, offset)[0]
        offset += 4
        if not 0 < abs(size) < 16384:
            raise ValueError(f'invalid name size at {offset - 4:x}')
        length = abs(size) * (2 if size < 0 else 1)
        raw = data[offset:offset + length]
        names.append(raw.decode('utf-16-le' if size < 0 else 'ascii').rstrip('\0'))
        offset += length + 8
    return names


def export_table(data, names):
    count, offset = struct.unpack_from('<II', data, 41)
    imports = []
    for i in range(count):
        values = struct.unpack_from('<7i', data, offset + i * 28)
        imports.append(names[values[5]])
    count, offset = struct.unpack_from('<II', data, 33)
    exports = []
    for i in range(count):
        values = struct.unpack_from('<17i', data, offset)
        assert 0 <= values[3] < len(names) and 0 <= values[11] < 1024
        exports.append(dict(index=i + 1, name=names[values[3]], number=values[4],
                            class_name=imports[-values[0] - 1] if values[0] < 0 else f'export:{values[0]}',
                            outer=values[2], size=values[8], offset=values[9]))
        offset += 68 + values[11] * 4
    assert offset == u32(data, 49), 'export table must end at DependsOffset'
    return exports


def skeleton_in_export(data, names, item, minimum_bones=20):
    # FMeshBone: FName, flags, quaternion, translation, child count, parent, trailing int.
    # Only accept arrays rooted at Scene_RootS0 and validate every record/tree edge.
    pattern = struct.pack('<II', names.index('Scene_RootS0'), 0)
    begin, end = item['offset'], item['offset'] + item['size']
    pos = begin
    found = []
    while True:
        pos = data.find(pattern, pos, end)
        if pos < 0:
            break
        count = u32(data, pos - 4)
        if minimum_bones <= count <= 512 and pos + count * 52 <= end:
            bones = []
            for i in range(count):
                values = struct.unpack_from('<III7fiii', data, pos + i * 52)
                ni, number, flags, *rest = values
                quat, translation, children, parent, extra = rest[:4], rest[4:7], rest[7], rest[8], rest[9]
                if not (ni < len(names) and number == 0 and flags == 0 and extra == -1 and
                        0 <= children < count and 0 <= parent <= max(0, i - 1) and
                        all(math.isfinite(v) and abs(v) < 10000 for v in quat + translation) and
                        abs(sum(v*v for v in quat) - 1) < 0.001):
                    break
                bones.append(dict(index=i, name=names[ni], parent_index=parent,
                                  children=children, orientation=quat, translation=translation))
            if len(bones) == count and len({b['name'] for b in bones}) == count:
                for bone in bones:
                    bone['parent'] = bones[bone['parent_index']]['name'] if bone['index'] else None
                    actual = sum(b['parent_index'] == bone['index'] for b in bones[1:])
                    assert actual == bone['children'], 'child count disagrees with parent array'
                found.append(dict(count=count, array_offset=pos - 4, bones=bones))
        pos += 8
    assert len(found) == 1, f"{item['name']}: expected one validated reference skeleton, got {len(found)}"
    return found[0]


def mesh_metadata(data, names, exports, item, skeleton):
    pos = item['offset'] + 4  # Serialized UObject net index precedes property tags.
    while True:
        name = names[u32(data, pos)]
        pos += 8
        if name == 'None':
            break
        kind = names[u32(data, pos)]
        size = u32(data, pos + 8)
        pos += 16
        if kind in ('StructProperty', 'ByteProperty'):
            pos += 8
        elif kind == 'BoolProperty':
            pos += 1
        pos += size
        assert pos <= item['offset'] + item['size']
    # Native mesh prefix: bounds, material references, origin and rotation.
    material_count = u32(data, pos + 28)
    assert 0 < material_count < 1024
    refs = struct.unpack_from('<' + 'i' * material_count, data, pos + 32)
    assert all(0 < index <= len(exports) for index in refs)
    assert pos + 32 + material_count * 4 + 24 == skeleton['array_offset']
    return dict(native_data_offset=pos, materials=[exports[i-1]['name'] for i in refs])


def compare(sister, hero):
    a = {b['name']: b for b in sister['skeleton']['bones']}
    b = {b['name']: b for b in hero['skeleton']['bones']}
    common = sorted(a.keys() & b.keys())
    rows = []
    for name in common:
        x, y = a[name], b[name]
        dot = abs(sum(p*q for p, q in zip(x['orientation'], y['orientation'])))
        rows.append(dict(name=name, sister_parent=x['parent'], hero_parent=y['parent'],
                         sister_index=x['index'], hero_index=y['index'],
                         translation_delta=math.dist(x['translation'], y['translation']),
                         local_rotation_delta_degrees=math.degrees(2 * math.acos(min(1.0, dot)))))
    return dict(shared_count=len(common), sister_only=sorted(a.keys()-b.keys()),
                hero_only=sorted(b.keys()-a.keys()),
                parent_mismatches=[row for row in rows if row['sister_parent'] != row['hero_parent']],
                sister_materials=sister['metadata']['materials'],
                hero_materials=hero['metadata']['materials'], shared_bones=rows)


if __name__ == '__main__':
    folder = ROOT / 'test-install/WellingtonGame/CookedPC/OrganizedForCooking'
    packages = {}
    for filename in sys.argv[1:] or ['WeSISTER_01_SF.upk', 'WeHero_SF.upk']:
        plain = package_bytes(folder / filename)
        names = name_table(plain)
        exports = export_table(plain, names)
        meshes = []
        for item in exports:
            if item['class_name'] != 'SkeletalMesh':
                continue
            skeleton = skeleton_in_export(plain, names, item)
            meshes.append(dict(item, skeleton=skeleton, metadata=mesh_metadata(plain, names, exports, item, skeleton)))
        result = dict(package=filename, sha256=hashlib.sha256((folder/filename).read_bytes()).hexdigest(),
                      decoded_size=len(plain), meshes=meshes,
                      socket_exports=[e for e in exports if e['class_name'] == 'SkeletalMeshSocket'])
        packages[filename] = result
        output = ROOT / 'local' / (Path(filename).stem + '-skeleton.json')
        output.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(filename, [(m['name'], m['skeleton']['count']) for m in meshes], '->', output)
    if 'WeSISTER_01_SF.upk' in packages and 'WeHero_SF.upk' in packages:
        sister = packages['WeSISTER_01_SF.upk']['meshes'][0]
        hero = next(m for m in packages['WeHero_SF.upk']['meshes'] if m['name'] == 'MESH_HERO')
        result = compare(sister, hero)
        output = ROOT/'local/theresa-hero-skeleton-comparison.json'
        output.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print('Shared bones:', result['shared_count'], 'parent mismatches:', len(result['parent_mismatches']))
        print('Sister only:', result['sister_only'])
        print('Hero only:', result['hero_only'])
        print('Materials:', result['sister_materials'], 'vs', result['hero_materials'])
        for row in result['shared_bones']:
            if row['name'].startswith('WEAPON_'):
                print('Weapon focus:', row)
