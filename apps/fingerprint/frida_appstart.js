const LIB = "libQSEEComAPI.so";
const HAL = "vendor.oplus.hardware.biometrics.fingerprint@2.1-service_uff";

function log(s) { console.log(s); }
function cstr(p) { try { return p.readCString(); } catch (e) { return "?"; } }

function hookLib(lib) {
    log("[+] " + LIB + " base=" + lib.base);
    const funcs = ["QSEECom_start_app", "QSEECom_start_app_V2",
                   "QSEECom_shutdown_app", "QSEECom_app_load_query",
                   "QSEECom_send_modified_cmd"];
    for (const n of funcs) {
        const p = lib.findExportByName(n);
        if (!p) { log("[-] " + n + " missing"); continue; }
        Interceptor.attach(p, {
            onEnter(args) {
                if (n === "QSEECom_shutdown_app") {
                    log("[QSEE] " + n + " handle=" + args[0]);
                    return;
                }
                if (n === "QSEECom_send_modified_cmd") {
                    log("[QSEE] " + n + " handle=" + args[0] +
                        " send_len=" + args[2].toInt32() +
                        " rsp_len=" + args[4].toInt32());
                    return;
                }
                log("[QSEE] " + n + " path='" + cstr(args[1]) +
                    "' ta='" + cstr(args[2]) +
                    "' arg3=" + args[3].toInt32() +
                    " handle_out=" + args[0]);
            },
            onLeave(retval) {
                log("[QSEE] " + n + " ret=" + retval.toInt32());
            }
        });
    }
}

const lib = Process.findModuleByName(LIB);
if (lib) {
    hookLib(lib);
} else {
    log("[*] waiting for " + LIB);
    const t = setInterval(() => {
        const m = Process.findModuleByName(LIB);
        if (m) { clearInterval(t); hookLib(m); }
    }, 100);
}

const hal = Process.findModuleByName(HAL);
if (hal) log("[+] HAL base=" + hal.base);
else log("[*] HAL module not resolved yet");
