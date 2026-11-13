import re,sys,zlib,hashlib,struct
txt=open(sys.argv[1]).read()
blocks=re.findall(r"```hex\n(.*?)```",txt,re.S)
bs=[bytes.fromhex(b.replace("\n"," ")) for b in blocks]
print([len(b) for b in bs])
h=bs[0]
print("hdr crc",hex(zlib.crc32(h[0:6]+h[8:len(h)-4])),h[-4:].hex())
off=len(h);prev=h[-4:]
for b in bs[1:]:
    so=struct.unpack_from("<Q",b,48)[0]
    print("blk off",off,"self",so,"prevok",b[56:60]==prev,"crc",hex(zlib.crc32(b[:-4])),b[-4:].hex(),"S",struct.unpack_from("<I",b,20)[0],len(b)-68)
    prev=b[-4:];off+=len(b)
f=b"".join(bs);print(len(f),hashlib.sha256(f[:6]+f[8:]).hexdigest())
# independent build
def cnt(v): return b"\x00" if v is None else b"\x01"+b"".join(struct.pack("<Q",x) for x in v)
def opt(v): return b"\x00" if v is None else b"\x01"+struct.pack("<Q",v)
def b16(s): return struct.pack("<H",len(s))+s
hd=struct.pack("<IHHIQQQ",0x54574750,5,0,0,0,0,0)+b"EVD1"
body=struct.pack("<HH",1,0)+b16(b"b")+b16(b"c")+struct.pack("<Q",0)+b"\x00"+b"\x01"+cnt([0]*15)+struct.pack("<I",1)+struct.pack("<IB",1,1)+b16(b"A")
H=len(hd)+4+len(body)+4
hh=hd+struct.pack("<I",H)+body
hh+=struct.pack("<I",zlib.crc32(hh[:6]+hh[8:]))
print("hdr eq",hh==h,H)
def blk(off,prev,typ,n,pay,ft=0,lt=0):
    x=struct.pack("<QQIIIHHQQQ",ft,lt,n,len(pay),len(pay),typ,0,0,0,off)+prev+b"\x00\x00\x00\x00"+pay
    return x+struct.pack("<I",zlib.crc32(x))
def rec(k,b): return bytes([k])+struct.pack("<I",len(b))+b
life=struct.pack("<QI",1,7)+opt(None)+struct.pack("<Q",0)+b"\x00"+opt(None)
att=struct.pack("<QI",2,7)+opt(None)+b"\x01"+struct.pack("<II",1,1)+cnt([0]*15)
p1=struct.pack("<HHI",1,0,2)+rec(1,life)+rec(2,att)
b1=blk(201,hh[-4:],2,2,p1)
p2=b"\x05"+struct.pack("<III",7,1,0)+b"\x03"+struct.pack("<Q",0)+b"\x00"
b2=blk(201+len(b1),b1[-4:],0,1,p2,5,5)
void=struct.pack("<Q",9)+cnt(None)+b16(b"x")+opt(0)
p3=struct.pack("<HHI",1,0,1)+rec(5,void)
b3=blk(201+len(b1)+len(b2),b2[-4:],2,1,p3)
print("eq",b1==bs[1],b2==bs[2],b3==bs[3])
