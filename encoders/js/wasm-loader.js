
let LAMEMOD=null;
async function lameInstance(){
  if(!LAMEMOD){const bytes=Uint8Array.from(atob(LAME_WASM_B64),c=>c.charCodeAt(0));LAMEMOD=await WebAssembly.compile(bytes);}
  let mem=null;const dv=()=>new DataView(mem.buffer);const imports={};
  for(const im of WebAssembly.Module.imports(LAMEMOD)){
    if(im.kind!=="function") continue;
    const ns=imports[im.module]||(imports[im.module]={});
    if(im.name==="fd_write") ns[im.name]=(fd,iovs,n,nw)=>{const d=dv();let t=0;for(let i=0;i<n;i++)t+=d.getUint32(iovs+i*8+4,true);d.setUint32(nw,t,true);return 0};
    else if(im.name==="proc_exit") ns[im.name]=c=>{throw new Error("LAME exit "+c)};
    else if(/_sizes_get$/.test(im.name)) ns[im.name]=(a,b)=>{const d=dv();d.setUint32(a,0,true);d.setUint32(b,0,true);return 0};
    else ns[im.name]=()=>0;
  }
  const inst=await WebAssembly.instantiate(LAMEMOD,imports);
  mem=inst.exports.memory; if(inst.exports._initialize) inst.exports._initialize();
  return inst.exports;
}
