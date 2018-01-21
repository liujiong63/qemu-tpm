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

#endif /* HW_ACPI_TPM_H */
