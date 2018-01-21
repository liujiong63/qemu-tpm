/*
 * tpm_crb.c - QEMU's TPM CRB interface emulator
 *
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * Authors:
 *   Marc-André Lureau <marcandre.lureau@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * tpm_crb is a device for TPM 2.0 Command Response Buffer (CRB) Interface
 * as defined in TCG PC Client Platform TPM Profile (PTP) Specification
 * Family “2.0” Level 00 Revision 01.03 v22
 */

#include "qemu/osdep.h"

#include "qemu-common.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "exec/address-spaces.h"

#include "hw/pci/pci_ids.h"
#include "hw/acpi/tpm.h"
#include "sysemu/tpm_backend.h"
#include "tpm_int.h"
#include "tpm_util.h"

typedef struct CRBState {
    SysBusDevice parent_obj;

    TPMBackend *tpmbe;
    TPMBackendCmd cmd;
    struct crb_regs regs;
    MemoryRegion mmio;
    MemoryRegion cmdmem;

    size_t be_buffer_size;
} CRBState;

#define CRB(obj) OBJECT_CHECK(CRBState, (obj), TYPE_TPM_CRB)

#define DEBUG_CRB 0

#define DPRINTF(fmt, ...) do {                  \
        if (DEBUG_CRB) {                        \
            printf(fmt, ## __VA_ARGS__);        \
        }                                       \
    } while (0)

#define CRB_ADDR_LOC_STATE offsetof(struct crb_regs, loc_state)
#define CRB_ADDR_LOC_CTRL offsetof(struct crb_regs, loc_ctrl)
#define CRB_ADDR_CTRL_REQ offsetof(struct crb_regs, ctrl_req)
#define CRB_ADDR_CTRL_CANCEL offsetof(struct crb_regs, ctrl_cancel)
#define CRB_ADDR_CTRL_START offsetof(struct crb_regs, ctrl_start)

#define CRB_INTF_TYPE_CRB_ACTIVE        (1 << 0)
#define CRB_INTF_VERSION_CRB            (1 << 4)
#define CRB_INTF_CAP_LOCALITY_0_ONLY    (0 << 8)
#define CRB_INTF_CAP_IDLE_FAST          (0 << 9)
#define CRB_INTF_CAP_XFER_SIZE_64       (3 << 11)
#define CRB_INTF_CAP_FIFO_NOT_SUPPORTED (0 << 13)
#define CRB_INTF_CAP_CRB_SUPPORTED      (1 << 14)
#define CRB_INTF_IF_SELECTOR_CRB        (1 << 17)
#define CRB_INTF_IF_SELECTOR_UNLOCKED   (0 << 19)

#define CRB_LOC_STATE_TPM_ESTABLISHED   (1 << 0)
#define CRB_LOC_STATE_LOC_ASSIGNED      (1 << 1)
#define CRB_LOC_STATE_TPM_REG_VALID_STS (1 << 7)

#define CRB_LOC_STS_GRANTED             (1 << 0)
#define CRB_LOC_STS_BEEN_SEIZED         (1 << 1)

#define CRB_CTRL_STS_TPM_STS            (1 << 0)
#define CRB_CTRL_STS_TPM_IDLE           (1 << 1)

#define CRB_CTRL_CMD_SIZE (TPM_CRB_ADDR_SIZE - sizeof(struct crb_regs))

enum crb_loc_ctrl {
    CRB_LOC_CTRL_REQUEST_ACCESS = BIT(0),
    CRB_LOC_CTRL_RELINQUISH = BIT(1),
    CRB_LOC_CTRL_SEIZE = BIT(2),
    CRB_LOC_CTRL_RESET_ESTABLISHMENT_BIT = BIT(3),
};

enum crb_ctrl_req {
    CRB_CTRL_REQ_CMD_READY = BIT(0),
    CRB_CTRL_REQ_GO_IDLE = BIT(1),
};

enum crb_start {
    CRB_START_INVOKE = BIT(0),
};

enum crb_cancel {
    CRB_CANCEL_INVOKE = BIT(0),
};

static const char *addr_desc(unsigned off)
{
    struct crb_regs crb;

    switch (off) {
#define CASE(field)                                                 \
    case offsetof(struct crb_regs, field) ...                       \
        offsetof(struct crb_regs, field) + sizeof(crb.field) - 1:   \
        return G_STRINGIFY(field);
        CASE(loc_state);
        CASE(reserved1);
        CASE(loc_ctrl);
        CASE(loc_sts);
        CASE(reserved2);
        CASE(intf_id_low);
        CASE(intf_id_high);
        CASE(ctrl_ext_low);
        CASE(ctrl_ext_high);
        CASE(ctrl_req);
        CASE(ctrl_sts);
        CASE(ctrl_cancel);
        CASE(ctrl_start);
        CASE(ctrl_int_enable);
        CASE(ctrl_int_sts);
        CASE(ctrl_cmd_size);
        CASE(ctrl_cmd_pa_low);
        CASE(ctrl_cmd_pa_high);
        CASE(ctrl_rsp_size);
        CASE(ctrl_rsp_pa_low);
        CASE(ctrl_rsp_pa_high);
#undef CASE
    }
    return NULL;
}

static uint64_t tpm_crb_mmio_read(void *opaque, hwaddr addr,
                                  unsigned size)
{
    CRBState *s = CRB(opaque);
    void *regs = (void *)&s->regs + (addr & ~3);
    unsigned offset = addr & 3;
    uint32_t val = *(uint32_t *)regs >> (8 * offset);

    DPRINTF("CRB read 0x" TARGET_FMT_plx ":%s len:%u val: 0x%" PRIx32 "\n",
            addr, addr_desc(addr), size, val);
    return val;
}

static void tpm_crb_mmio_write(void *opaque, hwaddr addr,
                               uint64_t val, unsigned size)
{
    CRBState *s = CRB(opaque);
    DPRINTF("CRB write 0x" TARGET_FMT_plx ":%s len:%u val:%" PRIu64 "\n",
            addr, addr_desc(addr), size, val);

    switch (addr) {
    case CRB_ADDR_CTRL_REQ:
        switch (val) {
        case CRB_CTRL_REQ_CMD_READY:
            s->regs.ctrl_sts &= ~CRB_CTRL_STS_TPM_IDLE;
            break;
        case CRB_CTRL_REQ_GO_IDLE:
            s->regs.ctrl_sts |= CRB_CTRL_STS_TPM_IDLE;
            break;
        }
        break;
    case CRB_ADDR_CTRL_CANCEL:
        if (val == CRB_CANCEL_INVOKE && s->regs.ctrl_start & CRB_START_INVOKE) {
            tpm_backend_cancel_cmd(s->tpmbe);
        }
        break;
    case CRB_ADDR_CTRL_START:
        if (val == CRB_START_INVOKE &&
            !(s->regs.ctrl_start & CRB_START_INVOKE)) {
            void *mem = memory_region_get_ram_ptr(&s->cmdmem);

            s->regs.ctrl_start |= CRB_START_INVOKE;
            s->cmd = (TPMBackendCmd) {
                .in = mem,
                .in_len = MIN(tpm_cmd_get_size(mem), s->be_buffer_size),
                .out = mem,
                .out_len = s->be_buffer_size,
            };

            tpm_backend_deliver_request(s->tpmbe, &s->cmd);
        }
        break;
    case CRB_ADDR_LOC_CTRL:
        switch (val) {
        case CRB_LOC_CTRL_RESET_ESTABLISHMENT_BIT:
            /* not loc 3 or 4 */
            break;
        case CRB_LOC_CTRL_RELINQUISH:
            break;
        case CRB_LOC_CTRL_REQUEST_ACCESS:
            s->regs.loc_sts = CRB_LOC_STS_GRANTED;
            s->regs.loc_state = CRB_LOC_STATE_LOC_ASSIGNED |
                CRB_LOC_STATE_TPM_REG_VALID_STS;
            break;
        }
        break;
    }
}

static const MemoryRegionOps tpm_crb_memory_ops = {
    .read = tpm_crb_mmio_read,
    .write = tpm_crb_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void tpm_crb_reset(DeviceState *dev)
{
    CRBState *s = CRB(dev);

    tpm_backend_reset(s->tpmbe);

    s->be_buffer_size = MIN(tpm_backend_get_buffer_size(s->tpmbe),
                            CRB_CTRL_CMD_SIZE);

    s->regs = (struct crb_regs) {
        .intf_id_low =
            CRB_INTF_TYPE_CRB_ACTIVE |
            CRB_INTF_VERSION_CRB |
            CRB_INTF_CAP_LOCALITY_0_ONLY |
            CRB_INTF_CAP_IDLE_FAST |
            CRB_INTF_CAP_XFER_SIZE_64 |
            CRB_INTF_CAP_FIFO_NOT_SUPPORTED |
            CRB_INTF_CAP_CRB_SUPPORTED |
            CRB_INTF_IF_SELECTOR_CRB |
            CRB_INTF_IF_SELECTOR_UNLOCKED |
            0b0001 << 24,
        .intf_id_high =
            PCI_VENDOR_ID_IBM |
            0b0001 << 16
        ,
        .ctrl_cmd_size = CRB_CTRL_CMD_SIZE,
        .ctrl_cmd_pa_low = TPM_CRB_ADDR_BASE + sizeof(struct crb_regs),
        .ctrl_rsp_size = CRB_CTRL_CMD_SIZE,
        .ctrl_rsp_pa_low = TPM_CRB_ADDR_BASE + sizeof(struct crb_regs),
    };

    tpm_backend_startup_tpm(s->tpmbe, s->be_buffer_size);
}

static void tpm_crb_request_completed(TPMIf *ti, int ret)
{
    CRBState *s = CRB(ti);

    s->regs.ctrl_start &= ~CRB_START_INVOKE;
    if (ret != 0) {
        s->regs.ctrl_sts |= CRB_CTRL_STS_TPM_STS;
    }
}

static enum TPMVersion tpm_crb_get_version(TPMIf *ti)
{
    CRBState *s = CRB(ti);

    return tpm_backend_get_tpm_version(s->tpmbe);
}

static const VMStateDescription vmstate_tpm_crb = {
    .name = "tpm-crb",
    .unmigratable = 1,
};

static Property tpm_crb_properties[] = {
    DEFINE_PROP_TPMBE("tpmdev", CRBState, tpmbe),
    DEFINE_PROP_END_OF_LIST(),
};

static void tpm_crb_realizefn(DeviceState *dev, Error **errp)
{
    CRBState *s = CRB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!tpm_find()) {
        error_setg(errp, "at most one TPM device is permitted");
        return;
    }
    if (!s->tpmbe) {
        error_setg(errp, "'tpmdev' property is required");
        return;
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &tpm_crb_memory_ops, s,
        "tpm-crb-mmio", sizeof(struct crb_regs));
    memory_region_init_ram(&s->cmdmem, OBJECT(s),
        "tpm-crb-cmd", CRB_CTRL_CMD_SIZE, errp);

    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_mmio_map(sbd, 0, TPM_CRB_ADDR_BASE);
    /* allocate ram in bios instead? */
    memory_region_add_subregion(get_system_memory(),
        TPM_CRB_ADDR_BASE + sizeof(struct crb_regs), &s->cmdmem);
}

static void tpm_crb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    TPMIfClass *tc = TPM_IF_CLASS(klass);

    dc->realize = tpm_crb_realizefn;
    dc->props = tpm_crb_properties;
    dc->reset = tpm_crb_reset;
    dc->vmsd  = &vmstate_tpm_crb;
    dc->user_creatable = true;
    tc->model = TPM_MODEL_TPM_CRB;
    tc->get_version = tpm_crb_get_version;
    tc->request_completed = tpm_crb_request_completed;
}

static const TypeInfo tpm_crb_info = {
    .name = TYPE_TPM_CRB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CRBState),
    .class_init  = tpm_crb_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_TPM_IF },
        { }
    }
};

static void tpm_crb_register(void)
{
    type_register_static(&tpm_crb_info);
}

type_init(tpm_crb_register)
