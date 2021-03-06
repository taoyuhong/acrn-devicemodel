/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*
 * Automotive IO Controller mediator virtualization.
 *
 * IOC mediator block diagram.
 * +------------+     +------------------+
 * |    IOC     |<--->|Native CBC cdevs  |
 * |            |     |                  |
 * |  mediator  |     |/dev/cbc-lifecycle|
 * |            |     |/dev/cbc-signals  |
 * |            |     |...               |
 * |            |     +------------------+
 * |            |     +------------+
 * |            |     |Virtual UART|
 * |            |     |            |
 * |            |<--->|            |
 * |            |     |            |
 * +------------+     +------------+
 *
 * IOC mediator data flow diagram.
 * +------+       +----------------+
 * |Core  |<------+Native CBC cdevs|
 * |      |       +----------------+
 * |thread|       +----------------+
 * |      |<------+Virtual UART    |
 * |      |       +----------------+
 * |      |       +----------------+
 * |      +------>|Tx/Rx queues    |
 * +------+       +----------------+
 *
 * +------+       +----------------+
 * |Rx    |<------+Rx queue        |
 * |      |       +----------------+
 * |thread|
 * |      |       +----------------+
 * |      +------>|Native CBC cdevs|
 * +------+       +----------------+
 *
 * +------+       +----------------+
 * |Tx    |<------+Tx queue        |
 * |      |       +----------------+
 * |thread|       |
 * |      |       +----------------+
 * |      +------>|Virtual UART    |
 * +------+       +----------------+
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <string.h>
#include <stdbool.h>
#include <types.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "ioc.h"

/* For debugging log to a file */
static int ioc_debug;
static FILE *dbg_file;
#define IOC_LOG_INIT do { if (ioc_debug) {\
	dbg_file = fopen("/tmp/ioc_log", "w+");\
if (!dbg_file)\
	printf("ioc log open failed\r\n"); else cbc_set_log_file(dbg_file);\
} } while (0)
#define IOC_LOG_DEINIT do { if (dbg_file) fclose(dbg_file); dbg_file = NULL;\
cbc_set_log_file(dbg_file);\
} while (0)
#define DPRINTF(format, arg...) \
do { if (ioc_debug && dbg_file) { fprintf(dbg_file, format, arg);\
	fflush(dbg_file); } } while (0)
#define	WPRINTF(format, arg...) printf(format, ##arg)

/*
 * For debugging only, to generate lifecycle, signal and oem-raw data
 * from PTY devices instead of native CBC cdevs.
 */
/* #define IOC_DUMMY */

/*
 * Type definition for thread function.
 */
typedef void* (*ioc_work)(void *arg);

/*
 * IOC mediator and virtual UART communication channel path,
 * comes from DM command line parameters.
 */
static char virtual_uart_path[32];

/* IOC boot reason(for S5)
 * comes from DM command line parameters.
 */
static uint32_t ioc_boot_reason;

/*
 * Dummy pty slave fd is to maintain the pty active,
 * to avoid EIO error when close the slave pty.
 */
#ifdef IOC_DUMMY
static int dummy0_sfd = -1;
static int dummy1_sfd = -1;
static int dummy2_sfd = -1;
#endif

/*
 * IOC channels definition.
 */
static struct ioc_ch_info ioc_ch_tbl[] = {
	{IOC_INIT_FD, IOC_NP_PMT,   IOC_NATIVE_PMT,	IOC_CH_OFF},
	{IOC_INIT_FD, IOC_NP_LF,    IOC_NATIVE_LFCC,	IOC_CH_ON },
	{IOC_INIT_FD, IOC_NP_SIG,   IOC_NATIVE_SIGNAL,	IOC_CH_ON },
	{IOC_INIT_FD, IOC_NP_ESIG,  IOC_NATIVE_ESIG,	IOC_CH_OFF},
	{IOC_INIT_FD, IOC_NP_DIAG,  IOC_NATIVE_DIAG,	IOC_CH_OFF},
	{IOC_INIT_FD, IOC_NP_DLT,   IOC_NATIVE_DLT,	IOC_CH_OFF},
	{IOC_INIT_FD, IOC_NP_LIND,  IOC_NATIVE_LINDA,	IOC_CH_OFF},
	{IOC_INIT_FD, IOC_NP_RAW0,  IOC_NATIVE_RAW0,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW1,  IOC_NATIVE_RAW1,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW2,  IOC_NATIVE_RAW2,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW3,  IOC_NATIVE_RAW3,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW4,  IOC_NATIVE_RAW4,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW5,  IOC_NATIVE_RAW5,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW6,  IOC_NATIVE_RAW6,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW7,  IOC_NATIVE_RAW7,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW8,  IOC_NATIVE_RAW8,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW9,  IOC_NATIVE_RAW9,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW10, IOC_NATIVE_RAW10,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_RAW11, IOC_NATIVE_RAW11,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_DP_NONE,  IOC_VIRTUAL_UART,	IOC_CH_ON},
#ifdef IOC_DUMMY
	{IOC_INIT_FD, IOC_NP_FLF,   IOC_NATIVE_DUMMY0,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_FSIG,  IOC_NATIVE_DUMMY1,	IOC_CH_ON},
	{IOC_INIT_FD, IOC_NP_FRAW,  IOC_NATIVE_DUMMY2,	IOC_CH_ON}
#endif
};

static struct cbc_signal cbc_tx_signal_table[] = {
	{(uint16_t)CBC_SIG_ID_VSWA,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VSPD,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VESP,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VECT,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VRGR,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VPS,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VPM,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VMD,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VIS,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VGP,	4,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VAG,	4,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VFS,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VFL,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VDTE,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWUB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWPB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWNB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWDB,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWVA,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWSCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWPLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWPCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWHB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWEB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWECB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWCLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_SWAMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSUB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSPB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP9B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP8B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP7B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP6B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP5B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP4B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP3B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP2B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP1B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSP0B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSNB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSVA,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RSSSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSSCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSRDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSPLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSPSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSOMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSHB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSHDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSENB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSEJB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSCLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRSAMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RVCS,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PSS,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PUB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PPB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP9B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP8B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP7B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP6B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP5B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP4B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP3B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP2B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP1B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PP0B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PNB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PVA,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PSCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PSRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PRDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PPLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PPSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_POMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PHMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PHDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PENB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PEJB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PCFB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PCLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PAMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSUB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSPB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP9B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP8B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP7B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP6B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP5B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP4B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP3B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP2B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP1B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSP0B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSNB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSVA,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSAMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSSCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSSRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSRDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSPLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSPSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSOMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSHMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSHDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSENB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSEJB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSCFB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LRSCLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DVA,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DECSP,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DECST,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DAMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DNB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DUB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DPB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP9B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP8B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP7B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP6B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP5B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP4B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP3B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP2B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP1B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DP0B,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSCB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSRB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DRDB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSTB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DPLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DPSB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DOMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DHMB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DHHB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DENB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DEJB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DCFB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DCLB,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSTG,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSRR,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSRF,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSLR,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSLF,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_DSEH,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_CSSRRW,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_CSSRR,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_CSSLRW,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_CSSLR,	2,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_ATEMP,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_ANSL,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_ALTI,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VSA,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LLAT,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LLON,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LALT,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LACC,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LHED,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LSPD,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LSRC,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_LSCT,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFB,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFL1,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFL2,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFL3,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFR1,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFR2,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDFR3,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRC,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRL1,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRL2,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRL3,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRR1,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRR2,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_PDRR3,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VXA,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VYA,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VZA,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_MBV,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_TSA,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_TSE,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_IACR,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_IWCR,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_IFCR,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_GYROX,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_GYROY,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_IAVB,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_IAVMJ,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RAV,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RMAX,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RMIN,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_ACCX,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_ACCY,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_ACCZ,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_MDS,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_FCP,	10,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_GYROZ,	16,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_IAVMN,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RTST,	32,	CBC_ACTIVE},
};

static struct cbc_signal cbc_rx_signal_table[] = {
	{(uint16_t)CBC_SIG_ID_STFR,	32,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_EGYO,	1,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_WACS,	3,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RIFC,	1,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RIWC,	1,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RIAC,	1,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RIVS,	1,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_RRMS,	8,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_MTAM,	1,	CBC_ACTIVE},
	{(uint16_t)CBC_SIG_ID_VICL,	8,	CBC_ACTIVE}
};

static struct cbc_group cbc_rx_group_table[] = {
	{(uint16_t)CBC_GRP_ID_0,	CBC_ACTIVE},
};

static struct cbc_group cbc_tx_group_table[] = {
	{(uint16_t)CBC_GRP_ID_LOC,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_PDF,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_PDR,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_VAC,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_GAS,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_IVR,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_IRM,	CBC_ACTIVE},
	{(uint16_t)CBC_GRP_ID_GAC,	CBC_ACTIVE}
};

static struct wlist_signal wlist_rx_signal_table[] = {
};

static struct wlist_signal wlist_tx_signal_table[] = {
	{(uint16_t)CBC_SIG_ID_ATEMP,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VSPD,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VESP,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VECT,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VRGR,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VGP,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VAG,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_VFS,	DEFAULT_WLIST_NODE},
	{(uint16_t)CBC_SIG_ID_ALTI,	DEFAULT_WLIST_NODE}
};

static struct wlist_group wlist_rx_group_table[] = {
};

static struct wlist_group wlist_tx_group_table[] = {
};

/*
 * Read data from the native CBC cdevs and virtual UART based on
 * IOC channel ID.
 */
static int
ioc_ch_recv(enum ioc_ch_id id, uint8_t *buf, size_t size)
{
	int fd;
	int count;

	fd = ioc_ch_tbl[id].fd;
	if (fd < 0 || !buf || size == 0)
		return -1;
	count = read(fd, buf, size);

	/*
	 * Currently epoll work mode is LT, so ignore EAGAIN error.
	 * If change epoll work mode to ET, need to handle EAGAIN.
	 */
	if (count < 0) {
		DPRINTF("ioc read bytes error:%s\r\n", strerror(errno));
		return -1;
	}
	return count;
}

/*
 * Write data to the native CBC cdevs and virtual UART based on
 * IOC channel ID.
 */
int
ioc_ch_xmit(enum ioc_ch_id id, const uint8_t *buf, size_t size)
{
	int count = 0;
	int fd, rc;

	fd = ioc_ch_tbl[id].fd;
	if (fd < 0 || !buf || size == 0)
		return -1;
	while (count < size) {
		rc = write(fd, (buf + count), (size - count));

		/*
		 * Currently epoll work mode is LT, so ignore EAGAIN error.
		 * If change epoll work mode to ET, need to handle EAGAIN.
		 */
		if (rc < 0) {
			DPRINTF("ioc write error:%s\r\n", strerror(errno));
			break;
		}
		count += rc;
	}
	return count;
}

/*
 * Open native CBC cdevs.
 */
static int
ioc_open_native_ch(const char *dev_name)
{
	int fd;

	if (!dev_name)
		return -1;
	fd = open(dev_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		DPRINTF("ioc open %s failed:%s\r\n", dev_name, strerror(errno));
	return fd;
}

/*
 * Open PTY master device for IOC mediator and the PTY slave device for virtual
 * UART. The pair(master/slave) can work as a communication channel between
 * IOC mediator and virtual UART.
 */
static int
ioc_open_virtual_uart(const char *dev_name)
{
	int fd;
	char *slave_name;
	struct termios attr;

	fd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		goto open_err;
	if (grantpt(fd) < 0)
		goto pty_err;
	if (unlockpt(fd) < 0)
		goto pty_err;
	slave_name = ptsname(fd);
	if (!slave_name)
		goto pty_err;
	if ((unlink(dev_name) < 0) && errno != ENOENT)
		goto pty_err;
	if (symlink(slave_name, dev_name) < 0)
		goto pty_err;
	if (chmod(dev_name, 0660) < 0)
		goto attr_err;
	if (tcgetattr(fd, &attr) < 0)
		goto attr_err;
	cfmakeraw(&attr);
	attr.c_cflag |= CLOCAL;
	if (tcsetattr(fd, TCSANOW, &attr) < 0)
		goto attr_err;
	return fd;

attr_err:
	unlink(dev_name);
pty_err:
	close(fd);
open_err:
	return -1;
}

/*
 * Open native CBC cdevs and virtual UART.
 */
static int
ioc_ch_init(struct ioc_dev *ioc)
{
	int i, fd;
	struct ioc_ch_info *chl;

	for (i = 0, chl = ioc_ch_tbl; i < ARRAY_SIZE(ioc_ch_tbl); i++, chl++) {
		if (chl->stat == IOC_CH_OFF)
			continue;

		switch (i) {
		case IOC_NATIVE_LFCC:
		case IOC_NATIVE_SIGNAL:
		case IOC_NATIVE_RAW0 ... IOC_NATIVE_RAW11:
			fd = ioc_open_native_ch(chl->name);
			break;
		case IOC_VIRTUAL_UART:
			fd = ioc_open_virtual_uart(virtual_uart_path);
			break;
#ifdef IOC_DUMMY
		/*
		 * TODO: check open if success for dummy fd
		 */
		case IOC_NATIVE_DUMMY0:
			fd = ioc_open_virtual_uart(chl->name);
			dummy0_sfd = open(chl->name, O_RDWR | O_NOCTTY |
					O_NONBLOCK);
		case IOC_NATIVE_DUMMY1:
			fd = ioc_open_virtual_uart(chl->name);
			dummy1_sfd = open(chl->name, O_RDWR | O_NOCTTY |
					O_NONBLOCK);
		case IOC_NATIVE_DUMMY2:
			fd = ioc_open_virtual_uart(chl->name);
			dummy2_sfd = open(chl->name, O_RDWR | O_NOCTTY |
					O_NONBLOCK);
			break;
#endif
		default:
			fd = -1;
			break;
		}

		/*
		 * Critical channels must open successfully
		 * if can not open lifecycle or virtual UART
		 * ioc needs to exit initilization with failure
		 */
		if (fd < 0 && (i == IOC_NATIVE_LFCC || i == IOC_VIRTUAL_UART))
			return -1;

		chl->fd = fd;
	}
	return 0;
}

/*
 * Close native CBC cdevs and virtual UART.
 */
static void
ioc_ch_deinit(void)
{
	int i;
	struct ioc_ch_info *chl = NULL;

	for (i = 0, chl = ioc_ch_tbl; i < ARRAY_SIZE(ioc_ch_tbl); i++, chl++) {
		if (chl->fd < 0)
			continue;

		/*
		 * No need to call EPOLL_CTL_DEL before close the fd, since
		 * epoll_wait thread should exit before all channels release.
		 */
		close(chl->fd);
		chl->fd = IOC_INIT_FD;
	}

#ifdef IOC_DUMMY
	close(dummy0_sfd);
	close(dummy1_sfd);
	close(dummy2_sfd);
#endif
}

/*
 * Called to put a cbc_request to a specific queue
 */
static void
cbc_request_enqueue(struct ioc_dev *ioc, struct cbc_request *req,
		enum cbc_queue_type qtype, bool to_head)
{
	pthread_cond_t *cond;
	pthread_mutex_t *mtx;
	struct cbc_qhead *qhead;

	if (!req)
		return;

	if (qtype == CBC_QUEUE_T_RX) {
		cond = &ioc->rx_cond;
		mtx = &ioc->rx_mtx;
		qhead = &ioc->rx_qhead;
	} else if (qtype == CBC_QUEUE_T_TX) {
		cond = &ioc->tx_cond;
		mtx = &ioc->tx_mtx;
		qhead = &ioc->tx_qhead;
	} else {
		cond = NULL;
		mtx = &ioc->free_mtx;
		qhead = &ioc->free_qhead;
	}

	pthread_mutex_lock(mtx);
	if (to_head)
		SIMPLEQ_INSERT_HEAD(qhead, req, me_queue);
	else
		SIMPLEQ_INSERT_TAIL(qhead, req, me_queue);
	if (cond != NULL)
		pthread_cond_signal(cond);
	pthread_mutex_unlock(mtx);
}

/*
 * Called to get a cbc_request from a specific queue,
 * due to rx and tx threads have implemented getting a cbc_request from
 * related queue and only core thread needs to dequque, so only supports
 * dequeue from the free queue.
 */
static struct cbc_request*
cbc_request_dequeue(struct ioc_dev *ioc, enum cbc_queue_type qtype)
{
	struct cbc_request *free = NULL;

	if (qtype == CBC_QUEUE_T_FREE) {
		pthread_mutex_lock(&ioc->free_mtx);
		if (!SIMPLEQ_EMPTY(&ioc->free_qhead)) {
			free = SIMPLEQ_FIRST(&ioc->free_qhead);
			SIMPLEQ_REMOVE_HEAD(&ioc->free_qhead, me_queue);
		}
		pthread_mutex_unlock(&ioc->free_mtx);
	}
	return free;
}

/*
 * Build a cbc_request with CBC link frame and add the cbc_request to
 * the rx queue tail.
 */
void
ioc_build_request(struct ioc_dev *ioc, int32_t link_len, int32_t srv_len)
{
	int i, pos;
	struct cbc_ring *ring = &ioc->ring;
	struct cbc_request *req;

	req = cbc_request_dequeue(ioc, CBC_QUEUE_T_FREE);
	if (!req) {
		WPRINTF(("ioc queue is full!!, drop the data\n\r"));
		return;
	}
	for (i = 0; i < link_len; i++) {
		pos = (ring->head + i) & (CBC_RING_BUFFER_SIZE - 1);

		req->buf[i] = ring->buf[pos];
	}
	req->srv_len = srv_len;
	req->link_len = link_len;
	cbc_request_enqueue(ioc, req, CBC_QUEUE_T_RX, false);
}

/*
 * Rx processing of the epoll kicks.
 */
static int
ioc_process_rx(struct ioc_dev *ioc, enum ioc_ch_id id)
{
	uint8_t c;

	/*
	 * Read virtual UART data byte one by one
	 * FIXME: if IOC DM can get several bytes one time
	 * then need to improve this
	 */
	if (ioc_ch_recv(id, &c, sizeof(c)) < 0)
		return -1;
	if (cbc_copy_to_ring(&c, 1, &ioc->ring) == 0)
		cbc_unpack_link(ioc);
	return 0;
}

/*
 * Tx processing of the epoll kicks.
 */
static int
ioc_process_tx(struct ioc_dev *ioc, enum ioc_ch_id id)
{
	int count;
	struct cbc_request *req;

	req = cbc_request_dequeue(ioc, CBC_QUEUE_T_FREE);
	if (!req) {
		WPRINTF("ioc free queue is full!!, drop the data\r\n");
		return -1;
	}

	/*
	 * The data from native CBC cdevs and each receiving can read a complete
	 * CBC service frame, so copy the bytes to the CBC service start
	 * position.
	 */
	count = ioc_ch_recv(id, req->buf + CBC_SRV_POS, CBC_MAX_SERVICE_SIZE);
	if (count <= 0) {
		cbc_request_enqueue(ioc, req, CBC_QUEUE_T_FREE, false);
		DPRINTF("ioc channel=%d,recv error\r\n", id);
		return -1;
	}

	/* Build a cbc_request and send it to Tx queue */
	req->srv_len = count;
	req->link_len = 0;
	req->rtype = CBC_REQ_T_PROT;
#ifdef IOC_DUMMY
	if (id == IOC_NATIVE_DUMMY0)
		req->id = IOC_NATIVE_LFCC;
	else if (id == IOC_NATIVE_DUMMY1)
		req->id = IOC_NATIVE_SIGNAL;
	else if (id == IOC_NATIVE_DUMMY2)
		req->id = IOC_NATIVE_RAW11;
	else
		req->id = id;
#else
	req->id = id;
#endif
	cbc_request_enqueue(ioc, req, CBC_QUEUE_T_TX, false);
	return 0;
}

/*
 * Core thread monitors epoll events of Rx and Tx directions
 * based on the channel id for different process.
 */
static void
ioc_dispatch(struct ioc_dev *ioc, struct ioc_ch_info *chl)
{
	switch (chl->id) {
	case IOC_NATIVE_LFCC:
	case IOC_NATIVE_SIGNAL:
	case IOC_NATIVE_RAW0 ... IOC_NATIVE_RAW11:
#ifdef IOC_DUMMY
	case IOC_NATIVE_DUMMY0:
	case IOC_NATIVE_DUMMY1:
	case IOC_NATIVE_DUMMY2:
#endif
		ioc_process_tx(ioc, chl->id);
		break;
	case IOC_VIRTUAL_UART:
		ioc_process_rx(ioc, chl->id);
		break;
	default:
		DPRINTF("ioc dispatch got wrong channel:%d\r\n", chl->id);
		break;
	}
}

/*
 * Handle EPOLLIN events for native CBC cdevs and virtual UART.
 */
static void *
ioc_core_thread(void *arg)
{
	int n, i;
	struct ioc_ch_info *chl;
	int chl_size = ARRAY_SIZE(ioc_ch_tbl);
	struct ioc_dev *ioc = (struct ioc_dev *) arg;
	struct epoll_event eventlist[IOC_MAX_EVENTS];

	/*
	 * Initialize epoll events.
	 * NOTE: channel size always same with events number, so that acess
	 * event by channel index.
	 */
	for (i = 0, chl = ioc_ch_tbl; i < chl_size; i++, chl++) {
		if (chl->fd > 0) {
			ioc->evts[i].events = EPOLLIN;
			ioc->evts[i].data.ptr = chl;
			if (epoll_ctl(ioc->epfd, EPOLL_CTL_ADD, chl->fd,
						&ioc->evts[i]) < 0)
				DPRINTF("ioc epoll ctl %s failed, error:%s\r\n",
						chl->name, strerror(errno));
		}
	}

	/* Start to epoll wait loop */
	for (;;) {
		n = epoll_wait(ioc->epfd, eventlist, IOC_MAX_EVENTS, -1);
		if (n < 0 && errno != EINTR) {
			DPRINTF("ioc epoll wait error:%s, exit ioc core\r\n",
					strerror(errno));
			goto exit;
		}
		for (i = 0; i < n; i++)
			ioc_dispatch(ioc, (struct ioc_ch_info *)
					eventlist[i].data.ptr);
	}
exit:
	return NULL;
}

/*
 * Rx thread waits for CBC requests of rx queue, if rx queue is not empty,
 * it wll get a cbc_request from rx queue and invokes cbc_rx_handler to process.
 */
static void *
ioc_rx_thread(void *arg)
{
	struct ioc_dev *ioc = (struct ioc_dev *) arg;
	struct cbc_request *req = NULL;
	struct cbc_pkt packet;
	int err;

	memset(&packet, 0, sizeof(packet));
	packet.cfg = &ioc->rx_config;
	packet.boot_reason = ioc_boot_reason;
	for (;;) {
		pthread_mutex_lock(&ioc->rx_mtx);
		while (SIMPLEQ_EMPTY(&ioc->rx_qhead)) {
			err = pthread_cond_wait(&ioc->rx_cond, &ioc->rx_mtx);
			assert(err == 0);
			if (ioc->closing)
				goto exit;
		}
		if (ioc->closing)
			goto exit;

		/* Get a cbc request from the queue head */
		req = SIMPLEQ_FIRST(&ioc->rx_qhead);
		SIMPLEQ_REMOVE_HEAD(&ioc->rx_qhead, me_queue);
		pthread_mutex_unlock(&ioc->rx_mtx);
		packet.req = req;

		/*
		 * Reset the queue type to free queue
		 * prepare for routing after main process
		 */
		packet.qtype = CBC_QUEUE_T_FREE;

		/* rx main process */
		ioc->ioc_dev_rx(&packet);

		/* Route the cbc_request */
		if (packet.qtype == CBC_QUEUE_T_TX)
			cbc_request_enqueue(ioc, req, CBC_QUEUE_T_TX, true);
		else
			cbc_request_enqueue(ioc, req, CBC_QUEUE_T_FREE, false);
	}
exit:
	pthread_mutex_unlock(&ioc->rx_mtx);
	return NULL;
}

/*
 * Tx thread waits for CBC requests of tx queue, if tx queue is not empty,
 * it wll get a CBC request from tx queue and invokes cbc_tx_handler to process.
 */
static void *
ioc_tx_thread(void *arg)
{
	struct ioc_dev *ioc = (struct ioc_dev *) arg;
	struct cbc_request *req = NULL;
	struct cbc_pkt packet;
	int err;

	memset(&packet, 0, sizeof(packet));
	packet.cfg = &ioc->tx_config;
	packet.boot_reason = ioc_boot_reason;
	for (;;) {
		pthread_mutex_lock(&ioc->tx_mtx);
		while (SIMPLEQ_EMPTY(&ioc->tx_qhead)) {
			err =  pthread_cond_wait(&ioc->tx_cond, &ioc->tx_mtx);
			assert(err == 0);
			if (ioc->closing)
				goto exit;
		}
		if (ioc->closing)
			goto exit;

		/* Get a cbc request from the queue head */
		req = SIMPLEQ_FIRST(&ioc->tx_qhead);
		SIMPLEQ_REMOVE_HEAD(&ioc->tx_qhead, me_queue);
		pthread_mutex_unlock(&ioc->tx_mtx);
		packet.req = req;

		/*
		 * Reset the queue type to free queue
		 * prepare for routing after main process
		 */
		packet.qtype = CBC_QUEUE_T_FREE;

		/* tx main process */
		ioc->ioc_dev_tx(&packet);

		/* Route the cbc_request */
		if (packet.qtype == CBC_QUEUE_T_RX)
			cbc_request_enqueue(ioc, req, CBC_QUEUE_T_RX, true);
		else
			cbc_request_enqueue(ioc, req, CBC_QUEUE_T_FREE, false);
	}
exit:
	pthread_mutex_unlock(&ioc->tx_mtx);
	return NULL;
}

/*
 * Stop all threads(core/rx/tx)
 */
static void
ioc_kill_workers(struct ioc_dev *ioc)
{
	ioc->closing = 1;

	/* Stop IOC core thread */
	close(ioc->epfd);
	ioc->epfd = IOC_INIT_FD;
	pthread_join(ioc->tid, NULL);

	/* Stop IOC rx thread */
	pthread_mutex_lock(&ioc->rx_mtx);
	pthread_cond_signal(&ioc->rx_cond);
	pthread_mutex_unlock(&ioc->rx_mtx);
	pthread_join(ioc->rx_tid, NULL);

	/* Stop IOC tx thread */
	pthread_mutex_lock(&ioc->tx_mtx);
	pthread_cond_signal(&ioc->tx_cond);
	pthread_mutex_unlock(&ioc->tx_mtx);
	pthread_join(ioc->tx_tid, NULL);

	/* Release the cond and mutex */
	pthread_mutex_destroy(&ioc->rx_mtx);
	pthread_cond_destroy(&ioc->rx_cond);
	pthread_mutex_destroy(&ioc->tx_mtx);
	pthread_cond_destroy(&ioc->tx_cond);
	pthread_mutex_destroy(&ioc->free_mtx);
}

static int
ioc_create_thread(const char *name, pthread_t *tid,
		ioc_work func, void *arg)
{
	if (pthread_create(tid, NULL, func, arg) != 0) {
		DPRINTF("%s", "ioc can not create thread\r\n");
		return -1;
	}
	pthread_setname_np(*tid, name);
	return 0;
}

/*
 * Check if current platform supports IOC or not.
 */
static int
ioc_is_platform_supported(void)
{
	struct stat st;

	/* The early signal channel will be created after cbc attached,
	 * if not, the current platform does not support IOC, exit IOC mediator.
	 */
	return stat(IOC_NP_ESIG, &st);
}

/*
 * To get IOC bootup reason and virtual UART path for communication
 * between IOC mediator and virtual UART.
 */
int
ioc_parse(const char *opts)
{
	char *tmp;
	char *param = strdup(opts);

	tmp = strtok(param, ",");
	snprintf(virtual_uart_path, sizeof(virtual_uart_path), "%s", param);
	if (tmp != NULL) {
		tmp = strtok(NULL, ",");
		ioc_boot_reason = strtoul(tmp, 0, 0);
	}
	free(param);
	return 0;
}

/*
 * IOC mediator main entry.
 */
struct ioc_dev *
ioc_init(void)
{
	int i;
	struct ioc_dev *ioc;

	IOC_LOG_INIT;

	if (ioc_is_platform_supported() != 0)
		goto ioc_err;

	/* Check IOC boot reason */
	if (ioc_boot_reason == 0)
		goto ioc_err;
	ioc = (struct ioc_dev *)calloc(1, sizeof(struct ioc_dev));
	if (!ioc)
		goto ioc_err;
	ioc->pool = (struct cbc_request *)calloc(IOC_MAX_REQUESTS,
			sizeof(struct cbc_request));
	ioc->evts = (struct epoll_event *)calloc(ARRAY_SIZE(ioc_ch_tbl),
			sizeof(struct epoll_event));
	if (!ioc->pool || !ioc->evts)
		goto alloc_err;

	/*
	 * IOC mediator needs to manage more than 15 channels with mass data
	 * transfer, to avoid blocking other mevent users, IOC mediator
	 * creates its own epoll in one separated thread.
	 */
	ioc->epfd = epoll_create1(0);
	if (ioc->epfd < 0)
		goto alloc_err;

	/*
	 * Put all buffered CBC requests on the free queue, the free queue is
	 * used to be a cbc_request buffer.
	 */
	SIMPLEQ_INIT(&ioc->free_qhead);
	pthread_mutex_init(&ioc->free_mtx, NULL);
	for (i = 0; i < IOC_MAX_REQUESTS; i++)
		SIMPLEQ_INSERT_TAIL(&ioc->free_qhead, ioc->pool + i, me_queue);

	/*
	 * Initialize native CBC cdev and virtual UART.
	 */
	if (ioc_ch_init(ioc) != 0)
		goto chl_err;

	/* Initlialize CBC rx/tx signal and group whitelists */
	wlist_init_signal(cbc_rx_signal_table, ARRAY_SIZE(cbc_rx_signal_table),
			wlist_rx_signal_table,
			ARRAY_SIZE(wlist_rx_signal_table));
	wlist_init_group(cbc_rx_group_table, ARRAY_SIZE(cbc_rx_group_table),
			wlist_rx_group_table,
			ARRAY_SIZE(wlist_rx_group_table));
	wlist_init_signal(cbc_tx_signal_table, ARRAY_SIZE(cbc_tx_signal_table),
			wlist_tx_signal_table,
			ARRAY_SIZE(wlist_tx_signal_table));
	wlist_init_group(cbc_tx_group_table, ARRAY_SIZE(cbc_tx_group_table),
			wlist_tx_group_table,
			ARRAY_SIZE(wlist_tx_group_table));

	/* Setup IOC rx members */
	snprintf(ioc->rx_name, sizeof(ioc->rx_name), "ioc_rx");
	ioc->ioc_dev_rx = cbc_rx_handler;
	pthread_cond_init(&ioc->rx_cond, NULL);
	pthread_mutex_init(&ioc->rx_mtx, NULL);
	SIMPLEQ_INIT(&ioc->rx_qhead);
	ioc->rx_config.cbc_sig_num = ARRAY_SIZE(cbc_rx_signal_table);
	ioc->rx_config.cbc_grp_num = ARRAY_SIZE(cbc_rx_group_table);
	ioc->rx_config.wlist_sig_num = ARRAY_SIZE(wlist_rx_signal_table);
	ioc->rx_config.wlist_grp_num = ARRAY_SIZE(wlist_rx_group_table);
	ioc->rx_config.cbc_sig_tbl = cbc_rx_signal_table;
	ioc->rx_config.cbc_grp_tbl = cbc_rx_group_table;
	ioc->rx_config.wlist_sig_tbl = wlist_rx_signal_table;
	ioc->rx_config.wlist_grp_tbl = wlist_rx_group_table;

	/* Setup IOC tx members */
	snprintf(ioc->tx_name, sizeof(ioc->tx_name), "ioc_tx");
	ioc->ioc_dev_tx = cbc_tx_handler;
	pthread_cond_init(&ioc->tx_cond, NULL);
	pthread_mutex_init(&ioc->tx_mtx, NULL);
	SIMPLEQ_INIT(&ioc->tx_qhead);
	ioc->tx_config.cbc_sig_num = ARRAY_SIZE(cbc_tx_signal_table);
	ioc->tx_config.cbc_grp_num = ARRAY_SIZE(cbc_tx_group_table);
	ioc->tx_config.wlist_sig_num = ARRAY_SIZE(wlist_tx_signal_table);
	ioc->tx_config.wlist_grp_num = ARRAY_SIZE(wlist_tx_group_table);
	ioc->tx_config.cbc_sig_tbl = cbc_tx_signal_table;
	ioc->tx_config.cbc_grp_tbl = cbc_tx_group_table;
	ioc->tx_config.wlist_sig_tbl = wlist_tx_signal_table;
	ioc->tx_config.wlist_grp_tbl = wlist_tx_group_table;

	/*
	 * Three threads are created for IOC work flow.
	 * Rx thread is responsible for writing data to native CBC cdevs.
	 * Tx thread is responsible for writing data to virtual UART.
	 * Core thread is responsible for reading data from native CBC cdevs
	 * and virtual UART.
	 */
	if (ioc_create_thread(ioc->rx_name, &ioc->rx_tid, ioc_rx_thread,
			(void *)ioc) < 0)
		goto work_err;
	if (ioc_create_thread(ioc->tx_name, &ioc->tx_tid, ioc_tx_thread,
			(void *)ioc) < 0)
		goto work_err;
	snprintf(ioc->name, sizeof(ioc->name), "ioc_core");
	if (ioc_create_thread(ioc->name, &ioc->tid, ioc_core_thread,
			(void *)ioc) < 0)
		goto work_err;

	return ioc;
work_err:
	pthread_mutex_destroy(&ioc->rx_mtx);
	pthread_cond_destroy(&ioc->rx_cond);
	pthread_mutex_destroy(&ioc->tx_mtx);
	pthread_cond_destroy(&ioc->tx_cond);
	ioc_kill_workers(ioc);
chl_err:
	ioc_ch_deinit();
	pthread_mutex_destroy(&ioc->free_mtx);
	close(ioc->epfd);
alloc_err:
	free(ioc->evts);
	free(ioc->pool);
	free(ioc);
ioc_err:
	IOC_LOG_DEINIT;
	DPRINTF("%s", "ioc mediator startup failed!!\r\n");
	return NULL;
}

/*
 * Called by DM in main entry.
 */
void
ioc_deinit(struct ioc_dev *ioc)
{
	if (!ioc) {
		DPRINTF("%s", "ioc deinit parameter is NULL\r\n");
		return;
	}
	ioc_kill_workers(ioc);
	ioc_ch_deinit();
	close(ioc->epfd);
	free(ioc->evts);
	free(ioc->pool);
	free(ioc);
	IOC_LOG_DEINIT;
}
