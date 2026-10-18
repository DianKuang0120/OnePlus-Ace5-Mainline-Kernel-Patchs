/*
 * Dump Qualcomm CCI writes from the Android camera provider.
 *
 * Usage (rooted Android, frida-server running):
 *   frida -U -n vendor.qti.camera.provider-service_64 -l cci-dump.js
 *
 * Then open the front camera.
 */
'use strict';

const IOCTL_TYPE_V = 0x56;
const CCI_NR = 0xd7;          /* BASE_VIDIOC_PRIVATE (192) + 23 */
const CMD_WRITE = 6;          /* MSM_CCI_I2C_WRITE */
const CMD_WRITE_SEQ = 7;
const CMD_WRITE_BURST = 8;

const ioctlPtr =
    Module.findGlobalExportByName ? Module.findGlobalExportByName('ioctl')
                                  : Module.getExportByName(null, 'ioctl');

function hex(v) { return '0x' + v.toString(16); }

Interceptor.attach(ioctlPtr, {
    onEnter(args) {
        const req = args[1].toUInt32();
        if (((req >>> 8) & 0xff) !== IOCTL_TYPE_V)
            return;
        console.log('VIOCTL req=' + req.toString(16));

        const ctrl = args[2];
        const cmd = ctrl.add(16).readS32();
        if (cmd !== CMD_WRITE && cmd !== CMD_WRITE_SEQ && cmd !== CMD_WRITE_BURST)
            return;

        const arr = ctrl.add(24).readPointer();
        const size = ctrl.add(32).readU32();
        const addrType = ctrl.add(36).readU32();
        const dataType = ctrl.add(40).readU32();
        let s = 'CCI cmd=' + cmd + ' size=' + size +
                ' addr_type=' + addrType + ' data_type=' + dataType + ':';

        /* cam_sensor_i2c_reg_array is 20 bytes on this kernel. */
        const n = Math.min(size, 128);
        for (let i = 0; i < n; i++) {
            const e = arr.add(i * 20);
            const a = e.readU32();
            const d = e.readU32();
            const mask = e.add(12).readU32();
            s += ' ' + hex(a) + '=' + hex(d);
            if (mask)
                s += '/m' + hex(mask);
        }
        console.log(s);
    }
});

console.log('CCI dump hook installed on ioctl @ ' + ioctlPtr);
