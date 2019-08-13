/*
 * SCLP early driver
 *
 * Copyright IBM Corp. 2013
 */

#define KMSG_COMPONENT "sclp_early"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/errno.h>
#include <asm/ctl_reg.h>
#include <asm/sclp.h>
#include <asm/ipl.h>
#include "sclp_sdias.h"
#include "sclp.h"

#define SCLP_CMDW_READ_SCP_INFO		0x00020001
#define SCLP_CMDW_READ_SCP_INFO_FORCED	0x00120001

struct read_info_sccb {
	struct	sccb_header header;	/* 0-7 */
	u16	rnmax;			/* 8-9 */
	u8	rnsize;			/* 10 */
	u8	_reserved0[24 - 11];	/* 11-15 */
	u8	loadparm[8];		/* 24-31 */
	u8	_reserved1[48 - 32];	/* 32-47 */
	u64	facilities;		/* 48-55 */
	u8	_reserved2[84 - 56];	/* 56-83 */
	u8	fac84;			/* 84 */
	u8	fac85;			/* 85 */
	u8	_reserved3[91 - 86];	/* 86-90 */
	u8	flags;			/* 91 */
	u8	_reserved4[100 - 92];	/* 92-99 */
	u32	rnsize2;		/* 100-103 */
	u64	rnmax2;			/* 104-111 */
	u8	_reserved5[4096 - 112];	/* 112-4095 */
} __packed __aligned(PAGE_SIZE);

static char sccb_early[PAGE_SIZE] __aligned(PAGE_SIZE) __initdata;
static unsigned int sclp_con_has_vt220 __initdata;
static unsigned int sclp_con_has_linemode __initdata;
static unsigned long sclp_hsa_size;
static struct sclp_ipl_info sclp_ipl_info;

u64 sclp_facilities;
u8 sclp_fac84;
unsigned long long sclp_rzm;
unsigned long long sclp_rnmax;

static int __init sclp_cmd_sync_early(sclp_cmdw_t cmd, void *sccb)
{
	int rc;

	__ctl_set_bit(0, 9);
	rc = sclp_service_call(cmd, sccb);
	if (rc)
		goto out;
	__load_psw_mask(PSW_DEFAULT_KEY | PSW_MASK_BASE | PSW_MASK_EA |
			PSW_MASK_BA | PSW_MASK_EXT | PSW_MASK_WAIT);
	local_irq_disable();
out:
	/* Contents of the sccb might have changed. */
	barrier();
	__ctl_clear_bit(0, 9);
	return rc;
}

static int __init sclp_read_info_early(struct read_info_sccb *sccb)
{
	int rc, i;
	sclp_cmdw_t commands[] = {SCLP_CMDW_READ_SCP_INFO_FORCED,
				  SCLP_CMDW_READ_SCP_INFO};

	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		do {
			memset(sccb, 0, sizeof(*sccb));
			sccb->header.length = sizeof(*sccb);
			sccb->header.function_code = 0x80;
			sccb->header.control_mask[2] = 0x80;
			rc = sclp_cmd_sync_early(commands[i], sccb);
		} while (rc == -EBUSY);

		if (rc)
			break;
		if (sccb->header.response_code == 0x10)
			return 0;
		if (sccb->header.response_code != 0x1f0)
			break;
	}
	return -EIO;
}

static void __init sclp_facilities_detect(struct read_info_sccb *sccb)
{
	if (sclp_read_info_early(sccb))
		return;

	sclp_facilities = sccb->facilities;
	sclp_fac84 = sccb->fac84;
	if (sccb->fac85 & 0x02)
		S390_lowcore.machine_flags |= MACHINE_FLAG_ESOP;
	sclp_rnmax = sccb->rnmax ? sccb->rnmax : sccb->rnmax2;
	sclp_rzm = sccb->rnsize ? sccb->rnsize : sccb->rnsize2;
	sclp_rzm <<= 20;

	/* Save IPL information */
	sclp_ipl_info.is_valid = 1;
	if (sccb->flags & 0x2)
		sclp_ipl_info.has_dump = 1;
	memcpy(&sclp_ipl_info.loadparm, &sccb->loadparm, LOADPARM_LEN);
}

bool __init sclp_has_linemode(void)
{
	return !!sclp_con_has_linemode;
}

bool __init sclp_has_vt220(void)
{
	return !!sclp_con_has_vt220;
}

unsigned long long sclp_get_rnmax(void)
{
	return sclp_rnmax;
}

unsigned long long sclp_get_rzm(void)
{
	return sclp_rzm;
}

/*
 * This function will be called after sclp_facilities_detect(), which gets
 * called from early.c code. The sclp_facilities_detect() function retrieves
 * and saves the IPL information.
 */
void __init sclp_get_ipl_info(struct sclp_ipl_info *info)
{
	*info = sclp_ipl_info;
}

static int __init sclp_cmd_early(sclp_cmdw_t cmd, void *sccb)
{
	int rc;

	do {
		rc = sclp_cmd_sync_early(cmd, sccb);
	} while (rc == -EBUSY);

	if (rc)
		return -EIO;
	if (((struct sccb_header *) sccb)->response_code != 0x0020)
		return -EIO;
	return 0;
}

static void __init sccb_init_eq_size(struct sdias_sccb *sccb)
{
	memset(sccb, 0, sizeof(*sccb));

	sccb->hdr.length = sizeof(*sccb);
	sccb->evbuf.hdr.length = sizeof(struct sdias_evbuf);
	sccb->evbuf.hdr.type = EVTYP_SDIAS;
	sccb->evbuf.event_qual = SDIAS_EQ_SIZE;
	sccb->evbuf.data_id = SDIAS_DI_FCP_DUMP;
	sccb->evbuf.event_id = 4712;
	sccb->evbuf.dbs = 1;
}

static int __init sclp_set_event_mask(struct init_sccb *sccb,
				      unsigned long receive_mask,
				      unsigned long send_mask)
{
	memset(sccb, 0, sizeof(*sccb));
	sccb->header.length = sizeof(*sccb);
	sccb->mask_length = sizeof(sccb_mask_t);
	sccb->receive_mask = receive_mask;
	sccb->send_mask = send_mask;
	return sclp_cmd_early(SCLP_CMDW_WRITE_EVENT_MASK, sccb);
}

static long __init sclp_hsa_size_init(struct sdias_sccb *sccb)
{
	sccb_init_eq_size(sccb);
	if (sclp_cmd_early(SCLP_CMDW_WRITE_EVENT_DATA, sccb))
		return -EIO;
	if (sccb->evbuf.blk_cnt != 0)
		return (sccb->evbuf.blk_cnt - 1) * PAGE_SIZE;
	return 0;
}

static long __init sclp_hsa_copy_wait(struct sccb_header *sccb)
{
	memset(sccb, 0, PAGE_SIZE);
	sccb->length = PAGE_SIZE;
	if (sclp_cmd_early(SCLP_CMDW_READ_EVENT_DATA, sccb))
		return -EIO;
	return (((struct sdias_sccb *) sccb)->evbuf.blk_cnt - 1) * PAGE_SIZE;
}

unsigned long sclp_get_hsa_size(void)
{
	return sclp_hsa_size;
}

static void __init sclp_hsa_size_detect(void *sccb)
{
	long size;

	/* First try synchronous interface (LPAR) */
	if (sclp_set_event_mask(sccb, 0, 0x40000010))
		return;
	size = sclp_hsa_size_init(sccb);
	if (size < 0)
		return;
	if (size != 0)
		goto out;
	/* Then try asynchronous interface (z/VM) */
	if (sclp_set_event_mask(sccb, 0x00000010, 0x40000010))
		return;
	size = sclp_hsa_size_init(sccb);
	if (size < 0)
		return;
	size = sclp_hsa_copy_wait(sccb);
	if (size < 0)
		return;
out:
	sclp_hsa_size = size;
}

static unsigned int __init sclp_con_check_linemode(struct init_sccb *sccb)
{
	if (!(sccb->sclp_send_mask & (EVTYP_OPCMD_MASK | EVTYP_PMSGCMD_MASK)))
		return 0;
	if (!(sccb->sclp_receive_mask & (EVTYP_MSG_MASK | EVTYP_PMSGCMD_MASK)))
		return 0;
	return 1;
}

static void __init sclp_console_detect(struct init_sccb *sccb)
{
	if (sccb->header.response_code != 0x20)
		return;

	if (sccb->sclp_send_mask & EVTYP_VT220MSG_MASK)
		sclp_con_has_vt220 = 1;

	if (sclp_con_check_linemode(sccb))
		sclp_con_has_linemode = 1;
}

void __init sclp_early_detect(void)
{
	void *sccb = &sccb_early;

	sclp_facilities_detect(sccb);
	sclp_hsa_size_detect(sccb);

	/* Turn off SCLP event notifications.  Also save remote masks in the
	 * sccb.  These are sufficient to detect sclp console capabilities.
	 */
	sclp_set_event_mask(sccb, 0, 0);
	sclp_console_detect(sccb);
}
