/* ===== AAC (FDK AAC, AAC-LC) -> M4A ===== */
let AACMOD=null;
async function aacInstance(){
  if(!AACMOD){const bytes=Uint8Array.from(atob(AAC_WASM_B64),c=>c.charCodeAt(0));AACMOD=await WebAssembly.compile(bytes);}
  let mem=null;const dv=()=>new DataView(mem.buffer);const imports={};
  for(const im of WebAssembly.Module.imports(AACMOD)){
    if(im.kind!=="function") continue;
    const ns=imports[im.module]||(imports[im.module]={});
    if(im.name==="fd_write") ns[im.name]=(fd,iovs,n,nw)=>{const d=dv();let t=0;for(let i=0;i<n;i++)t+=d.getUint32(iovs+i*8+4,true);d.setUint32(nw,t,true);return 0};
    else if(im.name==="proc_exit") ns[im.name]=c=>{throw new Error("AAC exit "+c)};
    else if(/_sizes_get$/.test(im.name)) ns[im.name]=(a,b)=>{const d=dv();d.setUint32(a,0,true);d.setUint32(b,0,true);return 0};
    else ns[im.name]=()=>0;
  }
  const inst=await WebAssembly.instantiate(AACMOD,imports);
  mem=inst.exports.memory; if(inst.exports._initialize) inst.exports._initialize();
  return inst.exports;
}
const AAC_RATES=[8000,11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000];
class AacPipe{
  static async create(srcRate,nch,kbps,meta){
    if(!AAC_RATES.includes(srcRate)) throw new Error("تردد المصدر ("+srcRate+") غير مدعوم في AAC");
    const p=new AacPipe();
    p.x=await aacInstance(); p.sr=srcRate; p.nch=Math.min(2,Math.max(1,nch)); p.kbps=kbps; p.meta=meta||null;
    p.e=p.x.aac_init(srcRate,2,kbps,1);           // always stereo out, afterburner on (best quality)
    if(!p.e) throw new Error("تعذّرت تهيئة مرمِّز AAC بهذه الجودة");
    p.frameLen=p.x.aac_frame_len(p.e); p.delay=p.x.aac_delay(p.e);
    const al=p.x.aac_asc_len(p.e); p.asc=new Uint8Array(p.x.memory.buffer,p.x.aac_asc(p.e),al).slice();
    p.aus=[]; p.sizes=[]; p.inSamples=0; p.maxAU=0; p.total=0;
    return p;
  }
  take(k){
    if(k===-1) throw new Error("خطأ في ترميز AAC");
    if(k>0){ const au=new Uint8Array(this.x.memory.buffer,this.x.aac_out(this.e),k).slice(); this.aus.push(au); this.sizes.push(k); this.total+=k; if(k>this.maxAU) this.maxAU=k; }
  }
  push(chs){
    const L=chs[0], R=chs.length>1?chs[1]:chs[0];
    const n=L.length, STEP=32768;
    this.inSamples+=n;
    for(let o=0;o<n;o+=STEP){
      const m=Math.min(STEP,n-o);
      const ptr=this.x.aac_buf(this.e,m);
      const buf=new Int16Array(this.x.memory.buffer,ptr,m*2);
      for(let i=0;i<m;i++){
        let a=L[o+i], b=R[o+i];
        a=a>1?1:a<-1?-1:a; b=b>1?1:b<-1?-1:b;
        buf[2*i]=Math.round(a<0?a*32768:a*32767); buf[2*i+1]=Math.round(b<0?b*32768:b*32767);
      }
      let off=0, guard=0;
      while(off<m){
        const k=this.x.aac_encode(this.e,off,m-off,0);
        this.take(k);
        const c=this.x.aac_consumed();
        off+=c;
        if(c===0&&k<=0){ if(++guard>4) throw new Error("توقف مرمِّز AAC"); } else guard=0;
      }
    }
  }
  finish(){
    for(let i=0;i<10000;i++){ const k=this.x.aac_encode(this.e,0,0,1); if(k===-2) break; this.take(k); if(k===0) break; }
    this.x.aac_free(this.e);
    return buildM4A(this);
  }
}

/* --- minimal MP4/M4A writer (faststart: ftyp, moov, mdat) with iTunes-style metadata --- */
const enc8=new TextEncoder();
function u32(n){return new Uint8Array([(n>>>24)&255,(n>>>16)&255,(n>>>8)&255,n&255])}
function u16b(n){return new Uint8Array([(n>>>8)&255,n&255])}
function cc(s){const o=new Uint8Array(4);for(let i=0;i<4;i++)o[i]=s.charCodeAt(i)&255;return o}
function catb(...a){a=a.flat();const n=a.reduce((s,x)=>s+x.length,0),o=new Uint8Array(n);let p=0;for(const x of a){o.set(x,p);p+=x.length}return o}
function box(type,...parts){const body=catb(...parts);return catb(u32(body.length+8),cc(type),body)}
function fbox(type,ver,flags,...parts){return box(type,new Uint8Array([ver,(flags>>16)&255,(flags>>8)&255,flags&255]),...parts)}
function desc(tag,body){const n=body.length;return catb(new Uint8Array([tag,0x80|((n>>21)&127),0x80|((n>>14)&127),0x80|((n>>7)&127),n&127]),body)}
const MATRIX=catb(u32(0x00010000),u32(0),u32(0),u32(0),u32(0x00010000),u32(0),u32(0),u32(0),u32(0x40000000));
function ilstText(name,text){return box(name,box("data",u32(1),u32(0),enc8.encode(text)))}
function ilstFree(name,text){return box("----",fbox("mean",0,0,enc8.encode("com.apple.iTunes")),fbox("name",0,0,enc8.encode(name)),box("data",u32(1),u32(0),enc8.encode(text)))}
function buildIlst(meta){
  if(!meta) return null;
  const C="©"; const A=s=>String.fromCharCode(0xA9)+s; // ©xxx atoms (byte 0xA9)
  const items=[];
  if(meta.title) items.push(ilstText(A("nam"),meta.title));
  if(meta.artist){ items.push(ilstText(A("ART"),meta.artist)); items.push(ilstText("aART",meta.artist)); }
  if(meta.album) items.push(ilstText(A("alb"),meta.album));
  if(meta.publisher){ items.push(ilstText(A("wrt"),meta.publisher)); items.push(ilstText("cprt",meta.publisher)); items.push(ilstFree("PUBLISHER",meta.publisher)); }
  if(meta.year) items.push(ilstText(A("day"),meta.year));
  if(meta.genre) items.push(ilstText(A("gen"),meta.genre));
  if(meta.comment) items.push(ilstText(A("cmt"),meta.comment));
  if(meta.link) items.push(ilstFree("URL",meta.link));
  if(meta.track) items.push(box("trkn",box("data",u32(0),u32(0),new Uint8Array([0,0]),u16b(meta.track),u16b(meta.total||114),new Uint8Array([0,0]))));
  if(meta.cover&&meta.cover.bytes) items.push(box("covr",box("data",u32(meta.cover.mime==="image/png"?14:13),u32(0),meta.cover.bytes)));
  if(!items.length) return null;
  return box("udta",fbox("meta",0,0,fbox("hdlr",0,0,u32(0),cc("mdir"),cc("appl"),u32(0),u32(0),new Uint8Array([0])),box("ilst",...items)));
}
function buildM4A(p){
  const sr=p.sr, nAU=p.sizes.length, fl=p.frameLen;
  const mediaDur=nAU*fl;
  const playDur=Math.min(p.inSamples, Math.max(0,mediaDur-p.delay));
  const ftyp=box("ftyp",cc("M4A "),u32(0),cc("M4A "),cc("mp42"),cc("isom"));
  const esds=fbox("esds",0,0,desc(3,catb(u16b(1),new Uint8Array([0]),
      desc(4,catb(new Uint8Array([0x40,0x15]),new Uint8Array([(p.maxAU>>16)&255,(p.maxAU>>8)&255,p.maxAU&255]),u32(p.kbps*1000),u32(p.kbps*1000),desc(5,p.asc))),
      desc(6,new Uint8Array([2])))));
  const mp4a=box("mp4a",new Uint8Array(6),u16b(1),new Uint8Array(8),u16b(2),u16b(16),u16b(0),u16b(0),u32(sr<<16>>>0),esds);
  const stsz=fbox("stsz",0,0,u32(0),u32(nAU),...p.sizes.map(u32));
  const mk=(stcoOff)=>{
    const stbl=box("stbl",fbox("stsd",0,0,u32(1),mp4a),fbox("stts",0,0,u32(1),u32(nAU),u32(fl)),fbox("stsc",0,0,u32(1),u32(1),u32(nAU),u32(1)),stsz,fbox("stco",0,0,u32(1),u32(stcoOff)));
    const minf=box("minf",fbox("smhd",0,0,u16b(0),u16b(0)),box("dinf",fbox("dref",0,0,u32(1),fbox("url ",0,1))),stbl);
    const mdia=box("mdia",fbox("mdhd",0,0,u32(0),u32(0),u32(sr),u32(mediaDur),u16b(0x55c4),u16b(0)),
                    fbox("hdlr",0,0,u32(0),cc("soun"),u32(0),u32(0),u32(0),enc8.encode("SoundHandler\0")),minf);
    const edts=box("edts",fbox("elst",0,0,u32(1),u32(playDur),u32(p.delay),u16b(1),u16b(0)));
    const tkhd=fbox("tkhd",0,7,u32(0),u32(0),u32(1),u32(0),u32(playDur),u32(0),u32(0),u16b(0),u16b(0),u16b(0x0100),u16b(0),MATRIX,u32(0),u32(0));
    const trak=box("trak",tkhd,edts,mdia);
    const mvhd=fbox("mvhd",0,0,u32(0),u32(0),u32(sr),u32(playDur),u32(0x00010000),u16b(0x0100),new Uint8Array(10),MATRIX,new Uint8Array(24),u32(2));
    const ud=buildIlst(p.meta);
    return ud?box("moov",mvhd,trak,ud):box("moov",mvhd,trak);
  };
  let moov=mk(0);
  const off=ftyp.length+moov.length+8;
  moov=mk(off);
  return new Blob([ftyp,moov,u32(p.total+8),cc("mdat"),...p.aus],{type:"audio/mp4"});
}
