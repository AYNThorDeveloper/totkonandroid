#!/usr/bin/env python3
"""Build local, verified RomFS overrides for an original Options-menu row.
Game bytes remain under ignored android/build/. The verified source image is
never modified. Uses only Python's standard-library zstd implementation.
"""
import compression.zstd as zstd
import hashlib,json,struct,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
GAME=ROOT/'games/totk'
OUT=ROOT/'android/build/video-menu'
ASSETS=ROOT/'android/game_activity/build/assets/romfs-overlay'

def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def sarc_read(b):
    assert b[:4]==b'SARC' and b[6:8]==b'\xff\xfe'
    h=struct.unpack_from('<H',b,4)[0]; start=u32(b,12)
    assert u32(b,8)==len(b) and b[h:h+4]==b'SFAT'
    n=struct.unpack_from('<H',b,h+6)[0]; names=h+12+n*16+8
    result={}
    for i in range(n):
        _,attr,lo,hi=struct.unpack_from('<4I',b,h+12+i*16)
        p=names+(attr&0xffffff)*4
        name=b[p:b.index(0,p,start)].decode()
        assert name not in result and start+lo<=start+hi<=len(b)
        result[name]=b[start+lo:start+hi]
    return result

def sarc_write(entries):
    multiplier=0x65
    def hash_name(s):
        v=0
        for c in s.encode():v=(v*multiplier+c)&0xffffffff
        return v
    names=bytearray();nodes=[];payload=bytearray()
    for name,data in sorted(entries.items(),key=lambda e:(hash_name(e[0]),e[0])):
        while len(names)%4:names.append(0)
        attr=0x1000000+len(names)//4;names+=name.encode()+b'\0'
        # These overrides contain parameters/messages, no GPU textures.
        while len(payload)%8:payload.append(0)
        lo=len(payload);payload+=data;nodes.append((hash_name(name),attr,lo,len(payload)))
    table=b'SFAT'+struct.pack('<HHI',12,len(nodes),multiplier)+b''.join(struct.pack('<4I',*v) for v in nodes)
    table+=b'SFNT'+struct.pack('<HH',8,0)+names
    start=(20+len(table)+255)&~255
    header=b'SARC'+struct.pack('<HHIIHH',20,0xfeff,start+len(payload),start,0x100,0)
    result=header+table+bytes(start-20-len(table))+payload
    assert sarc_read(result)==entries
    return result

def append_option(b):
    assert b[:4]==b'YB\x07\0'
    def strings(at):
        return [b[at+u32(b,at+4+i*4):b.index(0,at+u32(b,at+4+i*4))].decode() for i in range(u32(b,at)>>8)]
    keys,values=strings(u32(b,4)),strings(u32(b,8))
    root=u32(b,12);assert b[root]==0xc1 and u32(b,root)>>8==1
    entry=root+4;assert keys[u32(b,entry)&0xffffff]=='OptionSetArray'
    arr=u32(b,entry+4);n=u32(b,arr)>>8;assert b[arr]==0xc0 and n==16
    at=(arr+4+n+3)&~3;old=[u32(b,at+i*4) for i in range(n)]
    data=bytearray(b)
    def align():
        while len(data)%4:data.append(0)
        return len(data)
    choices=align();data.extend(bytes([0xc0,1,0,0,0xa0,0,0,0])+struct.pack('<I',values.index('Normal')))
    new=align();data.extend(bytes([0xc1,3,0,0]))
    fields={'ButtonType':(0xa0,values.index('Selection')),'ChoiceType':(0xc0,choices),'OptionType':(0xa0,values.index('None'))}
    for key,(kind,value) in sorted(fields.items(),key=lambda e:keys.index(e[0])):
        data.extend(struct.pack('<II',keys.index(key)|(kind<<24),value))
    new_arr=align();data.extend(bytes([0xc0,n+1,0,0])+bytes([0xc1])*(n+1))
    align();data.extend(struct.pack('<%dI'%(n+1),*(old+[new])))
    new_root=align();data.extend(bytes([0xc1,1,0,0])+struct.pack('<II',keys.index('OptionSetArray')|(0xc0<<24),new_arr))
    struct.pack_into('<I',data,12,new_root)
    return bytes(data)

def msbt_append(b,extra):
    assert b[:8]==b'MsgStdBn' and b[8:10]==b'\xff\xfe' and b[12]==1 # UTF-16
    count=struct.unpack_from('<H',b,14)[0];pos=32;sections=[]
    for _ in range(count):
        magic=b[pos:pos+4];size=u32(b,pos+4);sections.append([magic,b[pos+16:pos+16+size]])
        pos=(pos+16+size+15)&~15
    sec=dict(sections);lbl=sec[b'LBL1'];txt=sec[b'TXT2'];n=u32(txt,0);labels={}
    for i in range(u32(lbl,0)):
        num,off=struct.unpack_from('<II',lbl,4+i*8)
        for _ in range(num):
            size=lbl[off];name=lbl[off+1:off+1+size].decode();value=u32(lbl,off+1+size);labels[name]=value;off+=1+size+4
    assert not set(extra)&labels.keys(), 'custom message ID collides with game data'
    offsets=[u32(txt,4+i*4) for i in range(n)]
    texts=[txt[o:offsets[i+1] if i+1<n else len(txt)] for i,o in enumerate(offsets)]
    for name,text in extra.items():labels[name]=len(texts);texts.append(text.encode('utf-16-le')+b'\0\0')
    buckets=[[] for _ in range(u32(lbl,0))]
    for name,index in labels.items():
        h=0
        for c in name.encode():h=(h*0x492+c)&0xffffffff
        buckets[h%len(buckets)].append(bytes([len(name)])+name.encode()+struct.pack('<I',index))
    new_lbl=bytearray(struct.pack('<I',len(buckets))+bytes(len(buckets)*8))
    for i,bucket in enumerate(buckets):
        struct.pack_into('<II',new_lbl,4+i*8,len(bucket),len(new_lbl));new_lbl+=b''.join(bucket)
    new_txt=bytearray(struct.pack('<I',len(texts))+bytes(len(texts)*4))
    for i,text in enumerate(texts):struct.pack_into('<I',new_txt,4+i*4,len(new_txt));new_txt+=text
    sec[b'LBL1']=bytes(new_lbl);sec[b'TXT2']=bytes(new_txt)
    if b'ATR1' in sec:
        attr=sec[b'ATR1'];old_n,entry_size=struct.unpack_from('<II',attr,0);assert old_n==n
        assert len(attr)==8+old_n*entry_size
        sec[b'ATR1']=struct.pack('<II',len(texts),entry_size)+attr[8:]+(attr[8:8+entry_size] if entry_size else b'')*len(extra)
    out=bytearray(b[:32])
    for magic,_ in sections:
        body=sec[magic];out+=magic+struct.pack('<I',len(body))+bytes(8)+body
        while len(out)%16:out.append(0xab)
    struct.pack_into('<I',out,18,len(out))
    return bytes(out)

RESTBL='System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs'
def restbl_grow(b,grown):
    """The game sizes each resource buffer from RESTBL; a grown file must get
    the same extra room or it fails to load (the Bootup pack then vanishes)."""
    assert b[:6]==b'RESTBL' and u32(b,6)==1
    slen,ncrc,nname=struct.unpack_from('<III',b,10)
    data=bytearray(b);crc={};pos=22
    for _ in range(ncrc):crc[u32(b,pos)]=pos;pos+=8
    names={}
    for _ in range(nname):names[b[pos:pos+slen].split(b'\0')[0].decode()]=pos+slen;pos+=slen+4
    assert pos==len(b)
    for name,delta in grown.items():
        at=names.get(name) or crc[zlib.crc32(name.encode())]+4
        struct.pack_into('<I',data,at,u32(data,at)+((delta+31)&~31))
    return bytes(data)

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    idx={e['path']:e for e in json.loads((GAME/'extracted/v143/romfs/index.json').read_text())['files']}
    image=open(GAME/'extracted/v143/romfs/romfs.bin','rb')
    def read(name):
        e=idx[name];image.seek(e['offset']);b=image.read(e['size']);assert len(b)==e['size'];return b
    dics={}
    for name,b in sarc_read(zstd.decompress(read('Pack/ZsDic.pack.zs'))).items():
        d=zstd.ZstdDict(b);dics[d.dict_id]=d
    files=[];grown={}
    for path in ['Pack/Bootup.Nin_NX_NVN.pack.zs']+sorted(p for p in idx if p.startswith('Mals/') and p.endswith('.sarc.zs')):
        original=read(path);info=zstd.get_frame_info(original);dic=dics.get(info.dictionary_id)
        entries=sarc_read(zstd.decompress(original,zstd_dict=dic))
        if path.startswith('Pack/'):
            member='UI/OptionSetTable/OptionSetTable.game__ui__OptionSetTable.bgyml';entries[member]=append_option(entries[member])
        else:
            members=[n for n in entries if n.endswith('LayoutMsg/SystemOption_00.msbt')]
            assert len(members)==1,members
            entries[members[0]]=msbt_append(entries[members[0]],{'0016':'Adjust frame rate and resolution for this device.','1016':'Video settings','1016_00':'Open'})
        raw=sarc_write(entries);size=len(zstd.decompress(original,zstd_dict=dic))
        if len(raw)>size:grown[path[:-3]]=len(raw)-size
        packed=zstd.compress(raw,level=12,zstd_dict=dic)
        dest=ASSETS/path;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(packed)
        files.append(dict(path=path,size=len(packed),sha256=hashlib.sha256(packed).hexdigest(),original_sha256=hashlib.sha256(original).hexdigest()))
    original=read(RESTBL);assert not zstd.get_frame_info(original).dictionary_id
    packed=zstd.compress(restbl_grow(zstd.decompress(original),grown),level=12)
    dest=ASSETS/RESTBL;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(packed)
    files.append(dict(path=RESTBL,size=len(packed),sha256=hashlib.sha256(packed).hexdigest(),original_sha256=hashlib.sha256(original).hexdigest()))
    (OUT/'manifest.json').write_text(json.dumps(files,indent=2)+'\n')
    header=['/* Generated private asset manifest; no game bytes. */','#pragma once',
        'typedef struct { const char *path, *sha256; unsigned long long size; } VideoMenuAsset;',
        'static const VideoMenuAsset video_menu_assets[] = {']
    header += ['    {"%s", "%s", %dULL},'%(e['path'],e['sha256'],e['size']) for e in files]
    header += ['};','#define VIDEO_MENU_ASSET_COUNT (sizeof video_menu_assets / sizeof *video_menu_assets)']
    (ROOT/'android/build/video_menu_assets.h').write_text('\n'.join(header)+'\n')
    print(json.dumps({'status':'pass','overlays':files}))
if __name__=='__main__':main()
