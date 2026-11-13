import struct, zlib, hashlib, json
u8=lambda x:struct.pack('<B',x); u16=lambda x:struct.pack('<H',x)
u32=lambda x:struct.pack('<I',x); u64=lambda x:struct.pack('<Q',x)
def crc(b):
    c=0xffffffff
    for x in b:
        c^=x
        for _ in range(8): c=(c>>1)^(0xedb88320 if c&1 else 0)
    return c^0xffffffff
assert crc(b'123456789')==0xcbf43926
b16=lambda s:u16(len(s))+s
def uleb(n):
    o=b''
    while True:
        x=n&0x7f; n>>=7
        if n: o+=bytes([x|0x80])
        else: return o+bytes([x])
def opt(v): return u8(0) if v is None else u8(1)+u64(v)
def counters(c): return u8(0) if c is None else u8(1)+b''.join(u64(v) for v in c)
gen=lambda pid,fork:u32(pid)+opt(fork)
# header
body=(u32(0x54574750)+u16(5)+u16(0)+u32(0)+u64(0)+u64(0)+u64(0)+b'EVD1')
hdr_fixed_len=None
rest=(u16(1)+u16(0)+b16(b'b')+b16(b'c')+u64(0)+u8(0)+u8(1)+counters([0]*15)
      +u32(1)+u32(1)+u8(1)+b16(b'A'))
H=len(body)+4+len(rest)+4
hd=body+u32(H)+rest
hcrc=crc(hd[:6]+hd[8:])
hd+=u32(hcrc)
assert len(hd)==H
def block(off,prev,typ,n,t0,t1,payload):
    h=u64(t0)+u64(t1)+u32(n)+u32(len(payload))+u32(len(payload))+u16(typ)+u16(0)+u64(0)+u64(0)+u64(off)+u32(prev)+u8(0)+b'\0\0\0'
    assert len(h)==64
    c=crc(h+payload)
    return h+payload+u32(c),c
def ev(recs):
    p=u16(1)+u16(0)+u32(len(recs)); offs=[]
    for k,b in recs:
        offs.append(len(p)); p+=u8(k)+u32(len(b))+b
    return p,offs
life=u64(1)+gen(7,None)+u64(0)+u8(0)+opt(None)
att=u64(2)+gen(7,None)+u8(1)+u32(1)+u32(1)+counters([0]*15)
p1,o1=ev([(1,life),(2,att)])
b1,c1=block(H,hcrc,2,2,0,0,p1)
off2=H+len(b1)
p2=uleb(5)+u32(7)+u32(1)+u32(0)+uleb(3)+u64(0)+uleb(0)
b2,c2=block(off2,c1,0,1,5,5,p2)
off3=off2+len(b2)
void=u64(9)+counters(None)+b16(b'x')+opt(0)
p3,o3=ev([(5,void)])
b3,c3=block(off3,c2,2,1,0,0,p3)
D=off3+len(b3)
f=hd+b1+b2+b3
dig=hashlib.sha256(f[:6]+f[8:D]).hexdigest()
for n,x in (('ex1',hd+b1),('ex2',b2),('ex3',b3)):
    open(n+'.bin','wb').write(x)
open('all.bin','wb').write(f)
v=dict(header_len=H,header_crc=hex(hcrc),ex1=dict(offset=H,len=len(b1),crc=hex(c1),self_offset=H,rec_offsets=o1),
 ex2=dict(offset=off2,len=len(b2),crc=hex(c2),self_offset=off2),ex3=dict(offset=off3,len=len(b3),crc=hex(c3),self_offset=off3,rec_offsets=o3),
 total=len(f),D=D,predecessor_digest_if_this_were_predecessor=dig)
json.dump(v,open('values.json','w'),indent=1); print(json.dumps(v,indent=1))
