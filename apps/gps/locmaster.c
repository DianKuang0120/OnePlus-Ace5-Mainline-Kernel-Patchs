/*
 * LD_PRELOAD shim for qmicli: register as QMI LOC master client before
 * qmicli sends LOC Start / Register Events.
 *
 * QMI LOC service 0x10, message 0x00BA, TLV 0x01 (uint32 key),
 * master key 0xBAABCDEF (from Qualcomm location-apis-qcom).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <glib.h>
#include <libqmi-glib.h>

typedef void (*start_fn)(QmiClientLoc *, QmiMessageLocStartInput *, guint,
                         GCancellable *, GAsyncReadyCallback, gpointer);
typedef void (*reg_fn)(QmiClientLoc *, QmiMessageLocRegisterEventsInput *,
                       guint, GCancellable *, GAsyncReadyCallback, gpointer);

extern guint8 qmi_client_get_cid(QmiClient *self);

static GMainLoop *master_loop;
static int master_done;

static void master_ready(GObject *source, GAsyncResult *res, gpointer user)
{
    GError *error = NULL;
    QmiMessage *resp;

    resp = qmi_device_command_full_finish(QMI_DEVICE(source), res, &error);
    if (!resp) {
        fprintf(stderr, "[locmaster] master request failed: %s\n",
                error ? error->message : "unknown");
        g_clear_error(&error);
    } else {
        gsize len = 0;
        const guint8 *raw = qmi_message_get_raw(resp, &len, &error);
        fprintf(stderr, "[locmaster] master response (%zu bytes):", len);
        for (gsize i = 0; i < len; i++)
            fprintf(stderr, " %02x", raw[i]);
        fprintf(stderr, "\n");
        qmi_message_unref(resp);
    }
    master_done = 1;
    g_main_loop_quit(master_loop);
}


static void send_start(QmiClientLoc *client)
{
    QmiClient *base = QMI_CLIENT(client);
    GObject *dev_obj = qmi_client_get_device(base);
    QmiDevice *device;
    QmiMessage *msg;
    GError *error = NULL;
    gsize off;

    if (!dev_obj)
        return;
    device = QMI_DEVICE(dev_obj);
    msg = qmi_message_new(QMI_SERVICE_LOC, qmi_client_get_cid(base), 2,
                          0x0022);
    if (!msg)
        return;

    off = qmi_message_tlv_write_init(msg, 0x01, &error);
    if (!off || !qmi_message_tlv_write_guint8(msg, 0, &error) ||
        !qmi_message_tlv_write_complete(msg, off, &error))
        goto out;
    off = qmi_message_tlv_write_init(msg, 0x10, &error);
    if (!off || !qmi_message_tlv_write_guint32(msg, QMI_ENDIAN_LITTLE, 1,
                                               &error) ||
        !qmi_message_tlv_write_complete(msg, off, &error))
        goto out;
    off = qmi_message_tlv_write_init(msg, 0x12, &error);
    if (!off || !qmi_message_tlv_write_guint32(msg, QMI_ENDIAN_LITTLE, 1,
                                               &error) ||
        !qmi_message_tlv_write_complete(msg, off, &error))
        goto out;
    off = qmi_message_tlv_write_init(msg, 0x13, &error);
    if (!off || !qmi_message_tlv_write_guint32(msg, QMI_ENDIAN_LITTLE, 1000,
                                               &error) ||
        !qmi_message_tlv_write_complete(msg, off, &error))
        goto out;

    master_loop = g_main_loop_new(NULL, FALSE);
    fprintf(stderr, "[locmaster] sending LOC start\n");
    qmi_device_command_full(device, msg, NULL, 10, NULL, master_ready, NULL);
    g_main_loop_run(master_loop);
    g_main_loop_unref(master_loop);
    master_loop = NULL;

out:
    if (error) {
        fprintf(stderr, "[locmaster] start build error: %s\n", error->message);
        g_clear_error(&error);
    }
    qmi_message_unref(msg);
}

static void register_master(QmiClientLoc *client)
{
    QmiClient *base = QMI_CLIENT(client);
    GObject *dev_obj;
    QmiDevice *device;
    QmiMessage *msg;
    GError *error = NULL;
    gsize off;

    if (!qmi_client_get_device)
        return;
    dev_obj = qmi_client_get_device(base);
    if (!dev_obj)
        return;
    device = QMI_DEVICE(dev_obj);

    msg = qmi_message_new(QMI_SERVICE_LOC, qmi_client_get_cid(base), 1,
                          0x00BA);
    if (!msg)
        return;
    off = qmi_message_tlv_write_init(msg, 0x01, &error);
    if (!off)
        goto out;
    if (!qmi_message_tlv_write_guint32(msg, QMI_ENDIAN_LITTLE, 0xBAABCDEF, &error))
        goto out;
    if (!qmi_message_tlv_write_complete(msg, off, &error))
        goto out;

    master_loop = g_main_loop_new(NULL, FALSE);
    master_done = 0;
    fprintf(stderr, "[locmaster] registering master client\n");
    qmi_device_command_full(device, msg, NULL, 10, NULL, master_ready, NULL);
    g_main_loop_run(master_loop);
    g_main_loop_unref(master_loop);
    master_loop = NULL;

out:
    if (error) {
        fprintf(stderr, "[locmaster] build error: %s\n", error->message);
        g_clear_error(&error);
    }
    qmi_message_unref(msg);
}

void qmi_client_loc_start(QmiClientLoc *client,
                          QmiMessageLocStartInput *input, guint timeout,
                          GCancellable *cancellable,
                          GAsyncReadyCallback callback, gpointer user_data)
{
    static start_fn real;
    if (!real)
        real = (start_fn)dlsym(RTLD_NEXT, "qmi_client_loc_start");
    register_master(client);
    real(client, input, timeout, cancellable, callback, user_data);
}

void qmi_client_loc_register_events(QmiClientLoc *client,
                                    QmiMessageLocRegisterEventsInput *input,
                                    guint timeout, GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    static reg_fn real;
    if (!real)
        real = (reg_fn)dlsym(RTLD_NEXT, "qmi_client_loc_register_events");
    register_master(client);
    send_start(client);
    qmi_message_loc_register_events_input_set_client_type (
        input, QMI_LOC_CLIENT_TYPE_AFW, NULL);
    qmi_message_loc_register_events_input_set_client_string_id (
        input, "qmicli", NULL);
    real(client, input, timeout, cancellable, callback, user_data);
}
