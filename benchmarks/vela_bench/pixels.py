# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Bounded, dependency-free PNG decoding and coordinate-pattern pixel oracle."""
from collections import Counter
from pathlib import Path
import struct
import zlib

from . import BenchError


def png_rgb(path):
    raw = Path(path).read_bytes()
    if len(raw) > 64 * 1024 * 1024 or raw[:8] != b'\x89PNG\r\n\x1a\n':
        raise BenchError('Invalid pattern PNG')
    offset, compressed, header, ended = 8, bytearray(), None, False
    while offset + 12 <= len(raw):
        size = struct.unpack_from('!I', raw, offset)[0]
        kind = raw[offset+4:offset+8]
        data = raw[offset+8:offset+8+size]
        if offset + size + 12 > len(raw) or zlib.crc32(kind + data) & 0xffffffff != struct.unpack_from('!I', raw, offset+8+size)[0]:
            raise BenchError('Truncated PNG or invalid CRC')
        if kind == b'IHDR':
            if header is not None or offset != 8 or size != 13: raise BenchError('Invalid PNG header')
            header = struct.unpack('!IIBBBBB', data)
        elif kind == b'IDAT': compressed.extend(data)
        elif kind == b'IEND':
            ended = True
            if size or offset + 12 != len(raw): raise BenchError('Trailing PNG data')
            break
        elif kind[0] & 32 == 0: raise BenchError('Unsupported critical PNG chunk')
        offset += size + 12
    if not ended or not header: raise BenchError('Incomplete PNG')
    width, height, depth, color, compression, filtering, interlace = header
    if (not 0 < width <= 8192 or not 0 < height <= 8192 or width*height > 16777216
            or depth != 8 or color not in (2, 6) or compression or filtering or interlace):
        raise BenchError('Pattern capture needs bounded non-interlaced 8-bit RGB/RGBA PNG')
    channels = 3 if color == 2 else 4
    stride = width * channels
    decoder = zlib.decompressobj()
    decoded = decoder.decompress(compressed, (stride+1)*height+1)
    if len(decoded) != (stride+1)*height or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise BenchError('PNG decompression length changed')
    previous, rgb = bytearray(stride), bytearray(width*height*3)
    for y in range(height):
        begin = y * (stride+1)
        mode = decoded[begin]
        if mode > 4: raise BenchError('Unknown PNG filter')
        row = bytearray(decoded[begin+1:begin+1+stride])
        if mode:
            for x in range(stride):
                left = row[x-channels] if x >= channels else 0
                up = previous[x]
                corner = previous[x-channels] if x >= channels else 0
                if mode == 1: value = left
                elif mode == 2: value = up
                elif mode == 3: value = (left+up)//2
                else:
                    p = left+up-corner
                    distances = (abs(p-left), abs(p-up), abs(p-corner))
                    value = (left,up,corner)[distances.index(min(distances))]
                row[x] = (row[x]+value) & 255
        destination = y*width*3
        if channels == 3: rgb[destination:destination+width*3] = row
        else:
            for x in range(width): rgb[destination+x*3:destination+x*3+3] = row[x*4:x*4+3]
        previous = row
    return width, height, rgb


def pattern_metrics(path, metadata, output_size, scale):
    width, height, rgb = png_rgb(path)
    if [width,height] != list(output_size): raise BenchError('Pattern capture is not physical output size')
    keys = ('logical_width','logical_height','buffer_width','buffer_height','scale120','sequence')
    if any(type(metadata.get(k)) is not int or metadata[k] <= 0 for k in keys):
        raise BenchError('Invalid native pattern geometry')
    if (metadata.get('event') != 'pattern' or metadata.get('method') != 'coordinate-pattern-v1'
            or metadata.get('protocol') != 'wayland' or metadata.get('viewport') is not True
            or metadata['scale120'] != round(scale*120)):
        raise BenchError('Pattern did not receive the requested fractional scale')
    bw, bh = metadata['buffer_width'], metadata['buffer_height']
    if (bw != (metadata['logical_width']*metadata['scale120']+60)//120
            or bh != (metadata['logical_height']*metadata['scale120']+60)//120
            or bw > width+2 or bh > height+2):
        raise BenchError('Pattern buffer/viewport does not fit the physical output')
    votes = Counter()
    # Voting only locates an unscaled coordinate field. The entire expected
    # buffer rectangle is checked below, never just its surviving good pixels.
    for y in range(0,height,4):
        for x in range(0,width,4):
            i=(y*width+x)*3
            r,g,b=rgb[i:i+3]
            px,py=r+256*(b>>4),g+256*(b&15)
            ox,oy=x-px,y-py
            if px < bw and py < bh and -2 <= ox <= width-bw+2 and -2 <= oy <= height-bh+2:
                votes[(ox,oy)]+=1
    if not votes: raise BenchError('No complete identified coordinate pattern')
    origin,count=votes.most_common(1)[0]
    if count < max(8,bw*bh//64): raise BenchError('Ambiguous or resampled coordinate pattern')
    corner = int(8*scale+1.999999)
    bad, checked, offscreen = 0, 0, 0
    bounds=[bw,bh,-1,-1]
    ox,oy=origin
    for y in range(bh):
        for x in range(bw):
            if (x < corner or x >= bw-corner) and (y < corner or y >= bh-corner): continue
            if not (0<=ox+x<width and 0<=oy+y<height):
                offscreen+=1;checked+=1
                continue
            i=((oy+y)*width+ox+x)*3
            expected=(x&255,y&255,((x>>8)&15)*16+((y>>8)&15))
            different=tuple(rgb[i:i+3]) != expected
            bad += different
            if not different:
                bounds=[min(bounds[0],x),min(bounds[1],y),max(bounds[2],x),max(bounds[3],y)]
            checked += 1
    return {'origin':list(origin),'buffer_size':[bw,bh],'checked_pixels':checked,
            'different_pixels':bad,'offscreen_pixels':offscreen,'excluded_corner_side_pixels':corner,
            'matching_coordinate_extent_pixels':[bounds[2]-bounds[0]+1,bounds[3]-bounds[1]+1],
            'geometry_verified':True,'pixel_equality':bad == 0 and offscreen == 0,
            'scope':'native-wayland-coordinate-pattern-only','capture_resampled_by_suite':False}
