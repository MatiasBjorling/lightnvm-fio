#ifndef _DFT_IOCTL_H_
#define _DFT_IOCTL_H_

#include <stdint.h>

#define LNVM_GET_BLOCK	0xc0104c40
#define LNVM_PUT_BLOCK	0xc0104c41
#define LNVM_PIO	0xc0404c42

struct dft_ioctl_io
{
	uint8_t opcode;
	uint8_t flags;
	uint16_t nppas;
	uint32_t rsvd2;
	uint64_t metadata;
	uint64_t addr;
	uint64_t ppas;
	uint32_t metadata_len;
	uint32_t data_len;
	uint64_t status;
	uint32_t result;
	uint32_t rsvd3[3];
};

#define NVM_BLK_BITS (16)
#define NVM_PG_BITS  (16)
#define NVM_SEC_BITS (8)
#define NVM_PL_BITS  (8)
#define NVM_LUN_BITS (8)
#define NVM_CH_BITS  (7)

struct ppa_addr {
	/* Generic structure for all addresses */
	union {
		struct {
			uint64_t blk		: NVM_BLK_BITS;
			uint64_t pg		: NVM_PG_BITS;
			uint64_t sec		: NVM_SEC_BITS;
			uint64_t pl		: NVM_PL_BITS;
			uint64_t lun		: NVM_LUN_BITS;
			uint64_t ch		: NVM_CH_BITS;
			uint64_t reserved	: 1;
		} g;

		uint64_t ppa;
	};
};

struct dft_ioctl_vblk {
	uint64_t ppa;
	uint16_t flags;
	uint16_t rsvd[3];
};

struct dft_block {
	struct ppa_addr ppa;
	unsigned int nppas;
};

#endif //_DFT_IOCTL_H_
