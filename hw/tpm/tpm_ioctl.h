/*
 * tpm_ioctl.h
 *
 * (c) Copyright IBM Corporation 2014, 2015.
 *
 * This file is licensed under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

#include <stdint.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/ioctl.h>

/*
 * Every response from a command involving a TPM command execution must hold
 * the ptm_res as the first element.
 * ptm_res corresponds to the error code of a command executed by the TPM.
 */

typedef uint32_t ptm_res;

/* PTM_GET_TPMESTABLISHED */
struct ptm_est {
    ptm_res tpm_result;
    unsigned char bit; /* TPM established bit */
};

/* PTM_RESET_PTMESTABLISHED: reset establishment bit */
struct ptm_reset_est {
    union {
        struct {
            uint8_t loc; /* locality to use */
        } req;
        struct {
            ptm_res tpm_result;
        } resp;
    } u;
};

/* PTM_INIT */
struct ptm_init {
    union {
        struct {
            uint32_t init_flags; /* see definitions below */
        } req;
        struct {
            ptm_res tpm_result;
        } resp;
    } u;
};

/* above init_flags */
#define INIT_FLAG_DELETE_VOLATILE (1 << 0)
    /* delete volatile state file after reading it */

/* PTM_SET_LOCALITY */
struct ptm_loc {
    union {
        struct {
            uint8_t loc; /* locality to set */
        } req;
        struct {
            ptm_res tpm_result;
        } resp;
    } u;
};

/* PTM_HASH_DATA: hash given data */
struct ptm_hdata {
    union {
        struct {
            uint32_t length;
            uint8_t data[4096];
        } req;
        struct {
            ptm_res tpm_result;
        } resp;
    } u;
};

/*
 * size of the TPM state blob to transfer; x86_64 can handle 8k,
 * ppc64le only ~7k; keep the response below a 4k page size
 */
#define STATE_BLOB_SIZE (3 * 1024)

/*
 * The following is the data structure to get state blobs from the TPM.
 * If the size of the state blob exceeds the STATE_BLOB_SIZE, multiple reads
 * with this ioctl and with adjusted offset are necessary. All bytes
 * must be transferred and the transfer is done once the last byte has been
 * returned.
 * It is possible to use the read() interface for reading the data; however,
 * the first bytes of the state blob will be part of the response to the ioctl();
 * a subsequent read() is only necessary if the total length (totlength) exceeds
 * the number of received bytes. seek() is not supported.
 */
struct ptm_getstate {
    union {
        struct {
            uint32_t state_flags; /* may be: STATE_FLAG_DECRYPTED */
            uint32_t type;        /* which blob to pull */
            uint32_t offset;      /* offset from where to read */
        } req;
        struct {
            ptm_res tpm_result;
            uint32_t state_flags; /* may be: STATE_FLAG_ENCRYPTED */
            uint32_t totlength;   /* total length that will be transferred */
            uint32_t length;      /* number of bytes in following buffer */
            uint8_t  data[STATE_BLOB_SIZE];
        } resp;
    } u;
};

/* TPM state blob types */
#define PTM_BLOB_TYPE_PERMANENT  1
#define PTM_BLOB_TYPE_VOLATILE   2
#define PTM_BLOB_TYPE_SAVESTATE  3

/* state_flags above : */
#define STATE_FLAG_DECRYPTED     1 /* on input:  get decrypted state */
#define STATE_FLAG_ENCRYPTED     2 /* on output: state is encrypted */

/*
 * The following is the data structure to set state blobs in the TPM.
 * If the size of the state blob exceeds the STATE_BLOB_SIZE, multiple
 * 'writes' using this ioctl are necessary. The last packet is indicated
 * by the length being smaller than the STATE_BLOB_SIZE.
 * The very first packet may have a length indicator of '0' enabling
 * a write() with all the bytes from a buffer. If the write() interface
 * is used, a final ioctl with a non-full buffer must be made to indicate
 * that all data were transferred (a write with 0 bytes would not work).
 */
struct ptm_setstate {
    union {
        struct {
            uint32_t state_flags; /* may be STATE_FLAG_ENCRYPTED */
            uint32_t type;        /* which blob to set */
            uint32_t length;      /* length of the data;
                                     use 0 on the first packet to
                                     transfer using write() */
            uint8_t data[STATE_BLOB_SIZE];
        } req;
        struct {
            ptm_res tpm_result;
        } resp;
    } u;
};

/*
 * PTM_GET_CONFIG: Data structure to get runtime configuration information
 * such as which keys are applied.
 */
struct ptm_getconfig {
    union {
        struct {
            ptm_res tpm_result;
            uint32_t flags;
        } resp;
    } u;
};

#define CONFIG_FLAG_FILE_KEY        0x1
#define CONFIG_FLAG_MIGRATION_KEY   0x2


typedef uint64_t ptm_cap;
typedef struct ptm_est ptm_est;
typedef struct ptm_reset_est ptm_reset_est;
typedef struct ptm_loc ptm_loc;
typedef struct ptm_hdata ptm_hdata;
typedef struct ptm_init ptm_init;
typedef struct ptm_getstate ptm_getstate;
typedef struct ptm_setstate ptm_setstate;
typedef struct ptm_getconfig ptm_getconfig;

/* capability flags returned by PTM_GET_CAPABILITY */
#define PTM_CAP_INIT               (1)
#define PTM_CAP_SHUTDOWN           (1<<1)
#define PTM_CAP_GET_TPMESTABLISHED (1<<2)
#define PTM_CAP_SET_LOCALITY       (1<<3)
#define PTM_CAP_HASHING            (1<<4)
#define PTM_CAP_CANCEL_TPM_CMD     (1<<5)
#define PTM_CAP_STORE_VOLATILE     (1<<6)
#define PTM_CAP_RESET_TPMESTABLISHED (1<<7)
#define PTM_CAP_GET_STATEBLOB      (1<<8)
#define PTM_CAP_SET_STATEBLOB      (1<<9)
#define PTM_CAP_STOP               (1<<10)
#define PTM_CAP_GET_CONFIG         (1<<11)

enum {
    PTM_GET_CAPABILITY     = _IOR('P', 0, ptm_cap),
    PTM_INIT               = _IOWR('P', 1, ptm_init),
    PTM_SHUTDOWN           = _IOR('P', 2, ptm_res),
    PTM_GET_TPMESTABLISHED = _IOR('P', 3, ptm_est),
    PTM_SET_LOCALITY       = _IOWR('P', 4, ptm_loc),
    PTM_HASH_START         = _IOR('P', 5, ptm_res),
    PTM_HASH_DATA          = _IOWR('P', 6, ptm_hdata),
    PTM_HASH_END           = _IOR('P', 7, ptm_res),
    PTM_CANCEL_TPM_CMD     = _IOR('P', 8, ptm_res),
    PTM_STORE_VOLATILE     = _IOR('P', 9, ptm_res),
    PTM_RESET_TPMESTABLISHED = _IOWR('P', 10, ptm_reset_est),
    PTM_GET_STATEBLOB      = _IOWR('P', 11, ptm_getstate),
    PTM_SET_STATEBLOB      = _IOWR('P', 12, ptm_setstate),
    PTM_STOP               = _IOR('P', 13, ptm_res),
    PTM_GET_CONFIG         = _IOR('P', 14, ptm_getconfig),
};
