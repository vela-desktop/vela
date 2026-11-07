# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recompute the declared native marker/grid detector from original PNGs."""
import re
from . import BenchError
from .pixels import png_rgb

# These are binary RGB samples, including control-byte values in the green
# channel. List each accepted byte explicitly rather than using text ranges.
_MAGENTA_CANDIDATE = re.compile(b''.join(
    b'[' + re.escape(bytes(range(channel - 20, channel + 21))) + b']'
    for channel in (208, 32, 208)))


def evidence(path):
    width,height,rgb=png_rgb(path)
    def pixel(x,y):
        if not 0<=x<width or not 0<=y<height:return (-999,)*3
        at=(y*width+x)*3;return tuple(rgb[at:at+3])
    def close(value,color):return all(abs(a-b)<=20 for a,b in zip(value,color))
    found=[]
    for match in _MAGENTA_CANDIDATE.finditer(rgb):
        if match.start()%3:continue
        p=match.start()//3;x,y=p%width,p//width
        if x+608>width or y+32>height:continue
        if (not close(pixel(x+15,y),(208,32,208)) or not close(pixel(x+16,y),(32,208,32))
                or not close(pixel(x+31,y),(32,208,32)) or not close(pixel(x+576,y),(32,64,224))
                or not close(pixel(x+592,y),(224,128,32)) or y and close(pixel(x,y-1),(208,32,208))
                or not close(pixel(x,y+31),(208,32,208)) or not close(pixel(x+592,y+31),(224,128,32))):continue
        frame=0
        for bit in range(32):
            color=pixel(x+32+bit*17+8,y+16)
            if all(v>200 for v in color):frame|=1<<bit
            elif not close(color,(32,40,48)):break
        else:found.append({'x':x,'y':y,'frame':frame,'marker':'vela-frame-id-v2'})
    if len(found)>16:raise BenchError('Ambiguous desktop pixel markers')
    tiles=[]
    for y in range(18):
        for x in range(32):
            r,g,b=pixel((2*x+1)*width//64,(2*y+1)*height//36);tiles.append((r<<16)|(g<<8)|b)
    rectangles={}
    for identity,color in ((0x715000,(200,80,120)),(0x715002,(200,120,80))):
        headers=[m for m in found if m['frame']==identity]
        if len(headers)!=1:continue
        candidates=[];needle=bytes(color);offset=0
        while True:
            offset=rgb.find(needle,offset)
            if offset<0:break
            at=offset;offset+=3
            if at%3:continue
            p=at//3;x,y=p%width,p//width
            if (pixel(x-1,y)==color or pixel(x,y-1)==color or x+16>width or y+16>height
                    or any(pixel(x+dx,y+dy)!=color for dx,dy in ((15,0),(0,15),(15,15),(8,8)))):continue
            candidates.append((x+8,y+8))
        if len(candidates)==1:
            header=headers[0];x,y=header['x']-16,header['y']-16;cx,cy=candidates[0]
            rectangles[str(identity)]=[x,y,cx-x+16,cy-y+16]
    return {'width':width,'height':height,'tiles':tiles,'markers':found,'rectangles':rectangles}
