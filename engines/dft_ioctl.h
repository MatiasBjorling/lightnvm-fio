#ifndef _DFT_IOCTL_H_
#define _DFT_IOCTL_H_

#include <stdint.h>

#define LNVM_PUT_BLOCK			21525
#define LNVM_GET_BLOCK			21526
#define LNVM_PIO			12345

typedef unsigned long long sector_t;

struct dft_lun_info {
	unsigned long n_vblocks;
	unsigned long n_pages_per_blk;
};

struct dft_block {
	unsigned long id;
	unsigned long owner_id;
	unsigned long nppas;
	unsigned long ppa_bitmap;
	sector_t bppa;
	void *priv;
	unsigned int vlun_id;
	uint8_t flags;
};

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


#endif //_DFT_IOCTL_H_
