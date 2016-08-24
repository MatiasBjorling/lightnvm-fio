/*
 * LightNVM engine
 *
 * IO engine that reads/writes from MTD character devices.
 *
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include "dft_ioctl.h"
#include "../fio.h"
#include "../verify.h"

struct fio_lightnvm_data {
	int pg_size;
	struct dft_block vblk[64];
};

struct fio_lightnvm_data d;

static int fio_lightnvm_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct dft_block *blk = &d.vblk[td->subjob_number];
	struct dft_ioctl_io io;
	int ret, i;
	struct ppa_addr ppa;
	struct ppa_addr ppas[16];

	memset(&io, 0, sizeof(io));
	ppa.ppa = 0;
	ppa.g.blk = 1;

	io.flags = 0;
	ppa = blk->ppa;

	if (io_u->ddir == DDIR_WRITE) {
		io.opcode = 1;

		for (i = 0; i < 16; i++) {
			ppa.g.sec = i % 4;
			ppa.g.pl = i / 4;
			ppas[i] = ppa;
		}
		io.ppas = (uint64_t)ppas;
		io.nppas = 16;

		/* next 64k will go to next page */
		blk->ppa.g.pg++;
	} else if (io_u->ddir == DDIR_READ) {
		unsigned long long sect = io_u->offset >> 12;

		io.opcode = 0;
		ppa.g.pg = sect / 16;
		ppa.g.pl = (sect % 16) / 4;
		ppa.g.sec = sect % 4;
	/*	printf("offset %llu %u %u %u\n", io_u->offset >> 12, ppa.g.pg, ppa.g.pl,
				ppa.g.sec);*/
		io.ppas = ppa.ppa;
		io.nppas = 1;
	} else
		return FIO_Q_COMPLETED;

	io.addr = (uint64_t)io_u->buf;
	io.data_len = io_u->buflen;

	fio_ro_check(td, io_u);

	ret = ioctl(f->fd, LNVM_PIO, &io);
	if (ret)
		printf("fail: %u\n", ret);

	return FIO_Q_COMPLETED;
}

static int fio_lightnvm_open_file(struct thread_data *td, struct fio_file *f)
{
	struct dft_block *blk;
	int ret;

	ret = generic_open_file(td, f);
	if (ret)
		return ret;

	blk = &d.vblk[td->subjob_number];

	if (blk->ppa.ppa == 0 && td->o.td_ddir == TD_DDIR_WRITE) {
		struct dft_ioctl_vblk vblk;
		struct ppa_addr ppa;

		ppa.ppa = 0;
		ppa.g.ch = ((td->subjob_number) % 16) * 4;
		ppa.g.lun = (td->subjob_number) / 16;

		vblk.ppa = ppa.ppa;
		vblk.flags = 0;

		printf("thread: %u %u ch: %u lun: %u try get\n", td->thread_number - 1, td->subjob_number, ppa.g.ch, ppa.g.lun);
		ret = ioctl(f->fd, LNVM_GET_BLOCK, &vblk);
		if (ret) {
			td_verror(td, ret, "block could not be get for vlun");
			goto err_close;
		}
		blk->ppa.ppa = vblk.ppa;
		printf("thread: %u %u ch: %u lun: %u blk: %u got\n", td->thread_number - 1, td->subjob_number, blk->ppa.g.ch, blk->ppa.g.lun, blk->ppa.g.blk);
	}

	return 0;
err_close:
	{
		int fio_unused ret;
		ret = generic_close_file(td, f);
		return 1;
	}
}

static int fio_lightnvm_close_file(struct thread_data *td, struct fio_file *f)
{
	struct dft_block *blk = &d.vblk[td->subjob_number];
	int ret;

	if (blk->ppa.ppa != 0 && td_trim(td)) {
		struct dft_ioctl_vblk vblk;

		vblk.ppa = blk->ppa.ppa;
		vblk.flags = 0;

		printf("thread: %u ch: %u lun: %u blk: %u put\n",
						td->subjob_number,
						blk->ppa.g.ch, blk->ppa.g.lun,
						blk->ppa.g.blk);
		ret = ioctl(f->fd, LNVM_PUT_BLOCK, &vblk);
		if (ret)
			td_verror(td, ret, "block could not be put for vlun");

		blk->ppa.ppa = 0;
	}

	return generic_close_file(td, f);
}

int fio_lightnvm_get_file_size(struct thread_data *td, struct fio_file *f)
{
	/* all 32 luns 4K secs * plane * pgs * pgs_per_blk * nr_luns */
	f->real_file_size = 16ul * 256ul * 1024ul * 64ul * 4096ul;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "lightnvm",
	.version	= FIO_IOOPS_VERSION,
	.queue		= fio_lightnvm_queue,
	.open_file	= fio_lightnvm_open_file,
	.close_file	= fio_lightnvm_close_file,
	.get_file_size	= fio_lightnvm_get_file_size,
	.flags		= FIO_RAWIO | FIO_SYNCIO | FIO_NOEXTEND | FIO_MEMALIGN,
};

static void fio_init fio_lightnvm_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_lightnvm_unregister(void)
{
	unregister_ioengine(&ioengine);
}
