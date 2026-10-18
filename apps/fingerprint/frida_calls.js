const LIB="libQSEEComAPI.so";
function cstr(p){try{return p.readCString();}catch(e){return "?";}}
function dump(p,n){try{const b=p.readByteArray(n);const u=new Uint8Array(b);let s="";for(let i=0;i<u.length;i++){if(i%16===0)s+="\n";s+=u[i].toString(16).padStart(2,"0")+" ";}return s;}catch(e){return "?";}}
function hook(lib){
 const start=lib.findExportByName("QSEECom_start_app");
 console.log("[+] lib="+lib.base+" start="+start);
 Interceptor.attach(start,{onEnter(args){
  const tid=this.threadId; console.log("[start] "+cstr(args[1])+" ta="+cstr(args[2]));
  Stalker.follow(tid,{transform(iterator){
   let ins;
   while((ins=iterator.next())!==null){
    if(ins.mnemonic==="blr"||ins.mnemonic==="bl"){
     const addr=ins.address; const m=ins.mnemonic;
     iterator.putCallout(function(ctx){
      console.log("[call "+m+" "+addr+"] x0="+ctx.x0+" x1="+ctx.x1+" x2="+ctx.x2+" x3="+ctx.x3);
      try{ if(ctx.x1 && (ctx.x1<0x100000000)) console.log("  s1="+cstr(ptr(ctx.x1))); }catch(e){}
     });
    }
    iterator.keep();
   }
  }});
 },onLeave(r){console.log("[start] ret="+r.toInt32()); try{Stalker.unfollow(this.threadId);Stalker.flush();}catch(e){}}});
}
const lib=Process.findModuleByName(LIB); if(lib)hook(lib); else {const t=setInterval(()=>{const m=Process.findModuleByName(LIB);if(m){clearInterval(t);hook(m);}},100);}
