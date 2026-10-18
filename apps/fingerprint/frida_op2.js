const LIB = "libQSEEComAPI.so";
function dump(p, n) {
    try { const b = p.readByteArray(n); const u = new Uint8Array(b); let s="";
        for (let i=0;i<u.length;i++){ if(i%16===0)s+="\n"; s+=u[i].toString(16).padStart(2,"0")+" "; }
        return s; } catch(e){ return "?"; }
}
function cstr(p){ try{return p.readCString();}catch(e){return "?";} }
let stalking = 0;
function hook(lib) {
    console.log("[+] lib base="+lib.base);
    const start = lib.findExportByName("QSEECom_start_app");
    const callpc = lib.base.add(0x8ed0);
    console.log("[+] start="+start+" op2call="+callpc);
    Interceptor.attach(start, {
        onEnter(args) {
            const tid = this.threadId;
            console.log("[start] tid="+tid+" path="+cstr(args[1])+" ta="+cstr(args[2])+" a3="+args[3].toInt32());
            stalking++;
            Stalker.follow(tid, {
                transform(iterator) {
                    let ins;
                    while ((ins = iterator.next()) !== null) {
                        if (ins.address.equals(callpc)) {
                            iterator.putCallout(function (ctx) {
                                console.log("[op2] x0="+ctx.x0+" x1="+ctx.x1+" x2="+ctx.x2+" x3="+ctx.x3);
                                console.log("[op2] x2mem="+dump(ptr(ctx.x2), 0x80));
                                try { console.log("[op2] ibstr="+cstr(ptr(ctx.x2).readPointer())); } catch(e){}
                            });
                        }
                        iterator.keep();
                    }
                }
            });
        },
        onLeave(retval) {
            console.log("[start] ret="+retval.toInt32());
            /* keep following for subsequent start_app calls */
        }
    });
}
const lib=Process.findModuleByName(LIB);
if(lib) hook(lib); else {
    const t=setInterval(()=>{const m=Process.findModuleByName(LIB); if(m){clearInterval(t);hook(m);}},100);
}
