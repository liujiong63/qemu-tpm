/*
 * QEMU PowerPC pSeries Logical Partition (aka sPAPR) hardware System Emulator
 *
 * PAPR Virtual TPM
 *
 * Copyright (c) 2015, 2017 IBM Corporation.
 *
 * Authors:
 *    Stefan Berger <stefanb@linux.vnet.ibm.com>
 *
 * This code is licensed under the GPL version 2 or later. See the
 * COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

#include "sysemu/tpm_backend.h"
#include "tpm_int.h"
#include "tpm_util.h"

#include "hw/ppc/spapr.h"
#include "hw/ppc/spapr_vio.h"

#define DEBUG_SPAPR_VTPM 0

#define DPRINTF(fmt, ...) do { \
    if (DEBUG_SPAPR_VTPM) { \
        printf(fmt, ## __VA_ARGS__); \
    } \
} while (0)

#define VIO_SPAPR_VTPM(obj) \
     OBJECT_CHECK(SPAPRvTPMState, (obj), TYPE_TPM_SPAPR)

typedef struct VioCRQ {
    uint8_t valid;  /* 0x80: cmd; 0xc0: init crq
                       0x81-0x83: CRQ message response */
    uint8_t msg;    /* see below */
    uint16_t len;   /* len of TPM request; len of TPM response */
    uint32_t data;  /* rtce_dma_handle when sending TPM request */
    uint64_t reserved;
} VioCRQ;

typedef union TPMSpaprCRQ {
    VioCRQ s;
    uint8_t raw[sizeof(VioCRQ)];
} TPMSpaprCRQ;

#define SPAPR_VTPM_VALID_INIT_CRQ_COMMAND  0xC0
#define SPAPR_VTPM_VALID_COMMAND           0x80
#define SPAPR_VTPM_MSG_RESULT              0x80

/* msg types for valid = SPAPR_VTPM_VALID_INIT_CRQ */
#define SPAPR_VTPM_INIT_CRQ_RESULT           0x1
#define SPAPR_VTPM_INIT_CRQ_COMPLETE_RESULT  0x2

/* msg types for valid = SPAPR_VTPM_VALID_CMD */
#define SPAPR_VTPM_GET_VERSION               0x1
#define SPAPR_VTPM_TPM_COMMAND               0x2
#define SPAPR_VTPM_GET_RTCE_BUFFER_SIZE      0x3
#define SPAPR_VTPM_PREPARE_TO_SUSPEND        0x4

/* response error messages */
#define SPAPR_VTPM_VTPM_ERROR                0xff

/* error codes */
#define SPAPR_VTPM_ERR_COPY_IN_FAILED        0x3
#define SPAPR_VTPM_ERR_COPY_OUT_FAILED       0x4

#define MAX_BUFFER_SIZE TARGET_PAGE_SIZE

typedef struct {
    VIOsPAPRDevice vdev;

    TPMSpaprCRQ crq; /* track single TPM command */

    uint8_t state;
#define SPAPR_VTPM_STATE_NONE         0
#define SPAPR_VTPM_STATE_EXECUTION    1
#define SPAPR_VTPM_STATE_COMPLETION   2

    unsigned char buffer[MAX_BUFFER_SIZE];

    TPMBackendCmd cmd;

    TPMBackend *be_driver;
    TPMVersion be_tpm_version;

    size_t be_buffer_size;

    bool run_bh_func; /* whether to run the BH function after resume */
} SPAPRvTPMState;

static void tpm_spapr_show_buffer(const unsigned char *buffer,
                                  size_t buffer_len, const char *string)
{
#if DEBUG_SPAPR_VTPM
    size_t i, len;

    len = MIN(tpm_cmd_get_size(buffer), buffer_len);
    printf("spapr_vtpm: %s length = %zu\n", string, len);
    for (i = 0; i < len; i++) {
        if (i && !(i % 16)) {
            printf("\n");
        }
        printf("%.2X ", buffer[i]);
    }
    printf("\n");
#endif
}

/*
 * Send a request to the TPM.
 */
static void tpm_spapr_tpm_send(SPAPRvTPMState *s)
{
    tpm_spapr_show_buffer(s->buffer, sizeof(s->buffer), "spapr_vtpm: Tx TPM");

    s->state = SPAPR_VTPM_STATE_EXECUTION;
    s->cmd = (TPMBackendCmd) {
        .locty = 0,
        .in = s->buffer,
        .in_len = MIN(tpm_cmd_get_size(s->buffer), sizeof(s->buffer)),
        .out = s->buffer,
        .out_len = sizeof(s->buffer),
    };

    tpm_backend_deliver_request(s->be_driver, &s->cmd);
}

static int tpm_spapr_process_cmd(SPAPRvTPMState *s, uint64_t dataptr)
{
    long rc;

    /* a max. of be_buffer_size bytes can be transported */
    rc = spapr_vio_dma_read(&s->vdev, dataptr,
                            s->buffer, s->be_buffer_size);
    if (rc) {
        error_report("tpm_spapr_got_payload: DMA read failure !\n");
    }
    /* let vTPM handle any malformed request */
    tpm_spapr_tpm_send(s);

    return rc;
}

static int tpm_spapr_do_crq(struct VIOsPAPRDevice *dev, uint8_t *crq_data)
{
    SPAPRvTPMState *s = VIO_SPAPR_VTPM(dev);
    TPMSpaprCRQ local_crq;
    TPMSpaprCRQ *crq = &s->crq; /* requests only */
    int rc;

    memcpy(&local_crq.raw, crq_data, sizeof(local_crq.raw));

    DPRINTF("VTPM: do_crq %02x %02x ...\n",
            local_crq.raw[0], local_crq.raw[1]);

    switch (local_crq.s.valid) {
    case SPAPR_VTPM_VALID_INIT_CRQ_COMMAND: /* Init command/response */

        /* Respond to initialization request */
        switch (local_crq.s.msg) {
        case SPAPR_VTPM_INIT_CRQ_RESULT:
            DPRINTF("vtpm_do_crq: SPAPR_VTPM_INIT_CRQ_RESULT\n");
            memset(local_crq.raw, 0, sizeof(local_crq.raw));
            local_crq.s.valid = SPAPR_VTPM_VALID_INIT_CRQ_COMMAND;
            local_crq.s.msg = SPAPR_VTPM_INIT_CRQ_RESULT;
            spapr_vio_send_crq(dev, local_crq.raw);
            break;

        case SPAPR_VTPM_INIT_CRQ_COMPLETE_RESULT:
            DPRINTF("vtpm_do_crq: SPAPR_VTPM_INIT_CRQ_COMP_RESULT\n");
            memset(local_crq.raw, 0, sizeof(local_crq.raw));
            local_crq.s.valid = SPAPR_VTPM_VALID_INIT_CRQ_COMMAND;
            local_crq.s.msg = SPAPR_VTPM_INIT_CRQ_COMPLETE_RESULT;
            spapr_vio_send_crq(dev, local_crq.raw);
            break;
        }

        break;
    case SPAPR_VTPM_VALID_COMMAND: /* Payloads */
        switch (local_crq.s.msg) {
        case SPAPR_VTPM_TPM_COMMAND:
            DPRINTF("vtpm_do_crq: got TPM command payload!\n");
            if (s->state == SPAPR_VTPM_STATE_EXECUTION)
                return H_BUSY;
            /* this crq is tracked */
            memcpy(crq->raw, crq_data, sizeof(crq->raw));

            rc = tpm_spapr_process_cmd(s, be32_to_cpu(crq->s.data));

            if (rc == H_SUCCESS) {
                crq->s.valid = be16_to_cpu(0);
            } else {
                local_crq.s.valid = SPAPR_VTPM_MSG_RESULT;
                local_crq.s.msg = SPAPR_VTPM_VTPM_ERROR;
                local_crq.s.data = cpu_to_be32(SPAPR_VTPM_ERR_COPY_IN_FAILED);
                spapr_vio_send_crq(dev, local_crq.raw);
            }
            break;

        case SPAPR_VTPM_GET_RTCE_BUFFER_SIZE:
            DPRINTF("vtpm_do_crq: resp: buffer size is %zu\n",
                    s->be_buffer_size);
            local_crq.s.msg |= SPAPR_VTPM_MSG_RESULT;
            local_crq.s.len = cpu_to_be16(s->be_buffer_size);
            spapr_vio_send_crq(dev, local_crq.raw);
            break;

        case SPAPR_VTPM_GET_VERSION:
            local_crq.s.msg |= SPAPR_VTPM_MSG_RESULT;
            local_crq.s.len = cpu_to_be16(0);
            switch (s->be_tpm_version) {
            case TPM_VERSION_UNSPEC:
                local_crq.s.data = cpu_to_be32(0);
                break;
            case TPM_VERSION_1_2:
                local_crq.s.data = cpu_to_be32(1);
                break;
            case TPM_VERSION_2_0:
                local_crq.s.data = cpu_to_be32(2);
                break;
            }
            DPRINTF("vtpm_do_crq: resp: version %u\n",
                    be32_to_cpu(local_crq.s.data));
            spapr_vio_send_crq(dev, local_crq.raw);
            break;

        case SPAPR_VTPM_PREPARE_TO_SUSPEND:
            DPRINTF("vtpm_do_crq: resp: prep to suspend\n");
            local_crq.s.msg |= SPAPR_VTPM_MSG_RESULT;
            spapr_vio_send_crq(dev, local_crq.raw);
            break;

        default:
            DPRINTF("vtpm_do_crq: Unknown message type %02x\n",
                    crq->s.msg);
        }
        break;
    default:
        DPRINTF("vtpm_do_crq: unknown CRQ %02x %02x ...\n",
                local_crq.raw[0], local_crq.raw[1]);
    };

    return H_SUCCESS;
}

static void _tpm_spapr_request_completed(SPAPRvTPMState *s)
{
    TPMSpaprCRQ *crq = &s->crq;
    uint32_t len;
    int rc;

    tpm_spapr_show_buffer(s->buffer, sizeof(s->buffer), "spapr_vtpm: Rx TPM");

    s->state = SPAPR_VTPM_STATE_COMPLETION;

    /* a max. of be_buffer_size bytes can be transported */
    len = MIN(tpm_cmd_get_size(s->buffer), s->be_buffer_size);
    rc = spapr_vio_dma_write(&s->vdev, be32_to_cpu(crq->s.data),
                             s->buffer, len);

    crq->s.valid = SPAPR_VTPM_MSG_RESULT;
    if (rc == H_SUCCESS) {
        crq->s.msg = SPAPR_VTPM_TPM_COMMAND | SPAPR_VTPM_MSG_RESULT;
        crq->s.len = cpu_to_be16(len);
    } else {
        error_report("%s: DMA write failure\n", __func__);
        crq->s.msg = SPAPR_VTPM_VTPM_ERROR;
        crq->s.len = cpu_to_be16(0);
        crq->s.data = cpu_to_be32(SPAPR_VTPM_ERR_COPY_OUT_FAILED);
    }

    rc = spapr_vio_send_crq(&s->vdev, crq->raw);
    if (rc) {
        error_report("%s: Error sending response\n", __func__);
    }
}

static void tpm_spapr_request_completed(TPMIf *ti)
{
    SPAPRvTPMState *s = VIO_SPAPR_VTPM(ti);

    _tpm_spapr_request_completed(s);
}

static int tpm_spapr_do_startup_tpm(SPAPRvTPMState *s, size_t buffersize)
{
    return tpm_backend_startup_tpm(s->be_driver, buffersize);
}

static void tpm_spapr_reset(VIOsPAPRDevice *dev)
{
    SPAPRvTPMState *s = VIO_SPAPR_VTPM(dev);

    s->state = SPAPR_VTPM_STATE_NONE;

    s->be_tpm_version = tpm_backend_get_tpm_version(s->be_driver);

    s->be_buffer_size = MAX(ROUND_UP(tpm_backend_get_buffer_size(s->be_driver),
                                     TARGET_PAGE_SIZE),
                            sizeof(s->buffer));

    tpm_backend_reset(s->be_driver);
    tpm_spapr_do_startup_tpm(s, s->be_buffer_size);
}

static enum TPMVersion tpm_spapr_get_version(TPMIf *ti)
{
    SPAPRvTPMState *s = VIO_SPAPR_VTPM(ti);

    if (tpm_backend_had_startup_error(s->be_driver)) {
        return TPM_VERSION_UNSPEC;
    }

    return tpm_backend_get_tpm_version(s->be_driver);
}

/* persistent state handling */

static int tpm_spapr_pre_save(void *opaque)
{
    SPAPRvTPMState *s = opaque;

    /*
     * Synchronize with backend completion.
     */
    s->run_bh_func = tpm_backend_wait_cmd_completed(s->be_driver);

    /*
     * we cannot deliver the results to the VM (in state
     * SPAPR_VTPM_STATE_EXECUTION) since DMA would touch VM memory
     */

    return 0;
}

static int tpm_spapr_post_load(void *opaque,
                               int version_id __attribute__((unused)))
{
    SPAPRvTPMState *s = opaque;

    if (s->run_bh_func) {
        /*
         * now we can deliver the results to the VM via DMA
         */
        _tpm_spapr_request_completed(s);
    }

    return 0;
}

static const VMStateDescription vmstate_spapr_vtpm = {
    .name = "tpm-spapr",
    .version_id = 1,
    .minimum_version_id = 0,
    .minimum_version_id_old = 0,
    .pre_save = tpm_spapr_pre_save,
    .post_load = tpm_spapr_post_load,
    .fields = (VMStateField[]) {
        /* Sanity check */
        VMSTATE_UINT32_EQUAL(vdev.reg, SPAPRvTPMState, NULL),
        VMSTATE_UINT32_EQUAL(vdev.irq, SPAPRvTPMState, NULL),

        /* General VIO device state */
        VMSTATE_UINT64(vdev.signal_state, SPAPRvTPMState),
        VMSTATE_UINT64(vdev.crq.qladdr, SPAPRvTPMState),
        VMSTATE_UINT32(vdev.crq.qsize, SPAPRvTPMState),
        VMSTATE_UINT32(vdev.crq.qnext, SPAPRvTPMState),

        VMSTATE_BUFFER(crq.raw, SPAPRvTPMState),
        VMSTATE_UINT8(state, SPAPRvTPMState),
        VMSTATE_BUFFER(buffer, SPAPRvTPMState),
        VMSTATE_BOOL(run_bh_func, SPAPRvTPMState),
        VMSTATE_END_OF_LIST(),
    }
};

static Property tpm_spapr_properties[] = {
    DEFINE_SPAPR_PROPERTIES(SPAPRvTPMState, vdev),
    DEFINE_PROP_TPMBE("tpmdev", SPAPRvTPMState, be_driver),
    DEFINE_PROP_END_OF_LIST(),
};

static void tpm_spapr_realizefn(VIOsPAPRDevice *dev, Error **errp)
{
    SPAPRvTPMState *s = VIO_SPAPR_VTPM(dev);

    if (!tpm_find()) {
        error_setg(errp, "at most one TPM device is permitted");
        return;
    }

    dev->crq.SendFunc = tpm_spapr_do_crq;

    if (!s->be_driver) {
        error_setg(errp, "'tpmdev' property is required");
        return;
    }
}

static void tpm_spapr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VIOsPAPRDeviceClass *k = VIO_SPAPR_DEVICE_CLASS(klass);
    TPMIfClass *tc = TPM_IF_CLASS(klass);

    k->realize = tpm_spapr_realizefn;
    k->reset = tpm_spapr_reset;
    k->dt_name = "vtpm";
    k->dt_type = "IBM,vtpm";
    k->dt_compatible = "IBM,vtpm";
    k->signal_mask = 0x00000001;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->props = tpm_spapr_properties;
    k->rtce_window_size = 0x10000000;
    dc->vmsd = &vmstate_spapr_vtpm;

    tc->model = TPM_MODEL_TPM_SPAPR;
    tc->get_version = tpm_spapr_get_version;
    tc->request_completed = tpm_spapr_request_completed;
}

static const TypeInfo tpm_spapr_info = {
    .name          = TYPE_TPM_SPAPR,
    .parent        = TYPE_VIO_SPAPR_DEVICE,
    .instance_size = sizeof(SPAPRvTPMState),
    .class_init    = tpm_spapr_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_TPM_IF },
        { }
    }
};

static void tpm_spapr_register_types(void)
{
    type_register_static(&tpm_spapr_info);
}

type_init(tpm_spapr_register_types)
