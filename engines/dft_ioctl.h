#ifndef _DFT_IOCTL_H_
#define _DFT_IOCTL_H_

#include <stdint.h>

#define LNVM_PUT_BLOCK			21525
#define LNVM_GET_BLOCK			21526

#define LNVM_GET_NPAGES			21530
#define LNVM_GET_NBLOCKS		21528
#define LNVM_GET_N_FREE_BLOCKS		21520

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

#endif //_DFT_IOCTL_H_
