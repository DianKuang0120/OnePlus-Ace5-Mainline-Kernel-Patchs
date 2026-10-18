/*
 * Dump the vendor fingerprint HAL's TA messages on Android.
 *
 * The uff HAL service is:
 *   vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff
 *
 * The function at module+0x77fbc is the HAL's sendMessage(this, msg, len).
 * It is called with the full Jiiov message:
 *
 *   +0x00 u32 type = 2
 *   +0x04 u32 command id (FP_CMD_*)
 *   +0x08 u32 status (out)
 *   +0x30 payload
 *
 * Attach with, for example:
 *   frida -U -n vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff \
 *         -l frida_dump.js
 *
 * Then start a fingerprint enroll/authenticate operation.  Replay the
 * captured command bytes through /dev/qcomtee_fp with fpctl raw.
 */

const MODULE = "vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff";
const SEND_MESSAGES = [0x77fbc, 0x7902c, 0x79b64];

function dumpHex(ptr, len, max) {
    if (!ptr || !len) return "";
    const n = Math.min(len, max);
    const buf = ptr.readByteArray(n);
    if (!buf) return "";
    const u8 = new Uint8Array(buf);
    let out = "";
    for (let i = 0; i < u8.length; i++) {
        if (i && (i % 16) === 0) out += "\n";
        out += u8[i].toString(16).padStart(2, "0") + " ";
    }
    return out;
}

function fields(ptr) {
    if (!ptr) return "";
    try {
        return "type=" + ptr.add(0).readU32() +
               " cmd=0x" + ptr.add(4).readU32().toString(16) +
               " status=0x" + ptr.add(8).readU32().toString(16);
    } catch (e) {
        return "";
    }
}

function hookModule(base) {
    console.log("[+] fingerprint HAL base = " + base);

    for (const off of SEND_MESSAGES) {
        Interceptor.attach(base.add(off), {
            onEnter(args) {
                const msg = args[1];
                const len = args[2].toInt32();
                console.log("\n[HAL send +0x" + off.toString(16) +
                            "] len=" + len + " " + fields(msg));
                if (len > 0 && len < 4096)
                    console.log(dumpHex(msg, len, 512));
            }
        });
    }

    const qsee = Process.findModuleByName("libQSEEComAPI.so");
    if (!qsee) {
        console.log("[-] libQSEEComAPI.so not found");
        return;
    }

    for (const name of ["QSEECom_send_cmd", "QSEECom_send_modified_cmd"]) {
        const p = qsee.findExportByName(name);
        if (!p) continue;
        Interceptor.attach(p, {
            onEnter(args) {
                console.log("\n[QSEE] " + name +
                            " send_len=" + args[2].toInt32() +
                            " rsp_len=" + args[4].toInt32());
                console.log(dumpHex(args[1], args[2].toInt32(), 256));
            },
            onLeave(retval) {
                console.log("[QSEE] ret=" + retval.toInt32());
            }
        });
    }
}

const mod = Process.findModuleByName(MODULE);
if (mod) {
    hookModule(mod.base);
} else {
    console.log("[*] waiting for " + MODULE);
    const timer = setInterval(() => {
        const m = Process.findModuleByName(MODULE);
        if (m) {
            clearInterval(timer);
            hookModule(m.base);
        }
    }, 200);
}
