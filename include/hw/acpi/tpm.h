/*
 * tpm.h - TPM ACPI definitions
 *
 * Copyright (C) 2014 IBM Corporation
 *
 * Authors:
 *  Stefan Berger <stefanb@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * Implementation of the TIS interface according to specs found at
 * http://www.trustedcomputinggroup.org
 *
 */
#ifndef HW_ACPI_TPM_H
#define HW_ACPI_TPM_H

#include "qemu/osdep.h"

#define TPM_TIS_ADDR_BASE           0xFED40000
#define TPM_TIS_ADDR_SIZE           0x5000

#define TPM_TIS_IRQ                 5

struct crb_regs {
    uint32_t loc_state;
    uint32_t reserved1;
    uint32_t loc_ctrl;
    uint32_t loc_sts;
    uint8_t reserved2[32];
    uint32_t intf_id_low;
    uint32_t intf_id_high;
    uint32_t ctrl_ext_low;
    uint32_t ctrl_ext_high;

    uint32_t ctrl_req;
    uint32_t ctrl_sts;
    uint32_t ctrl_cancel;
    uint32_t ctrl_start;
    uint32_t ctrl_int_enable;
    uint32_t ctrl_int_sts;
    uint32_t ctrl_cmd_size;
    uint32_t ctrl_cmd_pa_low;
    uint32_t ctrl_cmd_pa_high;
    uint32_t ctrl_rsp_size;
    uint32_t ctrl_rsp_pa_low;
    uint32_t ctrl_rsp_pa_high;
    uint8_t reserved3[0x10];
    uint8_t data_buffer[0x1000 - 0x80];
} QEMU_PACKED;

#define TPM_CRB_ADDR_BASE           0xFED40000
#define TPM_CRB_ADDR_SIZE           0x1000
#define TPM_CRB_ADDR_CTRL \
    (TPM_CRB_ADDR_BASE + offsetof(struct crb_regs, ctrl_req))

#define TPM_LOG_AREA_MINIMUM_SIZE   (64 * 1024)

#define TPM_TCPA_ACPI_CLASS_CLIENT  0
#define TPM_TCPA_ACPI_CLASS_SERVER  1

#define TPM2_ACPI_CLASS_CLIENT      0
#define TPM2_ACPI_CLASS_SERVER      1

#define TPM2_START_METHOD_MMIO      6
#define TPM2_START_METHOD_CRB       7

/*
 * Physical Presence Interface
 */
#define TPM_PPI_ADDR_SIZE           0x400
#define TPM_PPI_ADDR_BASE           0xFED45000

struct tpm_ppi {
    uint8_t ppin;            /*  0: set by BIOS */
    uint32_t ppip;           /*  1: set by ACPI; not used */
    uint32_t pprp;           /*  5: response from TPM; set by BIOS */
    uint32_t pprq;           /*  9: opcode; set by ACPI */
    uint32_t pprm;           /* 13: parameter for opcode; set by ACPI */
    uint32_t lppr;           /* 17: last opcode; set by BIOS */
    uint32_t fret;           /* 21: set by ACPI; not used */
    uint8_t res1;            /* 25: reserved */
    uint32_t res2[4];        /* 26: reserved */
    uint8_t  res3[214];      /* 42: reserved */
    uint8_t  func[256];      /* 256: per TPM function implementation flags;
                                     set by BIOS */
/* actions OS should take to transition to the pre-OS env.; bits 0, 1 */
#define TPM_PPI_FUNC_ACTION_SHUTDOWN   (1 << 0)
#define TPM_PPI_FUNC_ACTION_REBOOT     (2 << 0)
#define TPM_PPI_FUNC_ACTION_VENDOR     (3 << 0)
#define TPM_PPI_FUNC_ACTION_MASK       (3 << 0)
/* whether function is blocked by BIOS settings; bits 2, 3, 4 */
#define TPM_PPI_FUNC_NOT_IMPLEMENTED     (0 << 2)
#define TPM_PPI_FUNC_BIOS_ONLY           (1 << 2)
#define TPM_PPI_FUNC_BLOCKED             (2 << 2)
#define TPM_PPI_FUNC_ALLOWED_USR_REQ     (3 << 2)
#define TPM_PPI_FUNC_ALLOWED_USR_NOT_REQ (4 << 2)
#define TPM_PPI_FUNC_MASK                (7 << 2)
} QEMU_PACKED;

#define TPM_PPI_STRUCT_SIZE  sizeof(struct tpm_ppi)

#define TPM_PPI_VERSION_1_30 1

#endif /* HW_ACPI_TPM_H */
