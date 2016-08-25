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
	struct dft_thread threads[128];
};

struct fio_lightnvm_data d;

static int fio_lightnvm_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct dft_thread *th = &d.threads[td->subjob_number];
	struct dft_ioctl_io io;
	struct ppa_addr ppa, ppas[16];
	int ret, i;
	int blk_offset;

	blk_offset = io_u->offset >> 24; /* 16MB */
	ppa = th->ppas[blk_offset];

	memset(&io, 0, sizeof(io));
	io.flags = 0;

	if (io_u->ddir == DDIR_WRITE) {
		io.opcode = 1;

		for (i = 0; i < 16; i++) {
			ppa.g.sec = i % 4;
			ppa.g.pl = i / 4;
			ppas[i] = ppa;
		}
		io.ppas = (uint64_t)ppas;
		io.nppas = 16;
		io.flags = 0x2;

		/* next 64k will go to next page */
		th->ppas[blk_offset].g.pg++;
	} else if (io_u->ddir == DDIR_READ) {
		unsigned long long sec_offset = (io_u->offset % (1ULL << 24)) >> 12;
		int len = io_u->buflen >> 12;
		io.nppas = len;
		io.opcode = 0;

		if (len == 1) {
			ppa.g.pg = sec_offset / 16;
			ppa.g.pl = (sec_offset % 16) / 4;
			ppa.g.sec = sec_offset % 4;
		/*	printf("offset %llu %u %u %u\n", io_u->offset >> 12, ppa.g.pg, ppa.g.pl,
					ppa.g.sec);*/
			io.ppas = ppa.ppa;
		} else {
			for (i = 0; i < 16; i++) {
				ppa.g.sec = i % 4;
				ppa.g.pl = i / 4;
				//ppa.g.ch = 0;
				ppas[i] = ppa;
			}
			io.ppas = (uint64_t)ppas;
			io.flags = 0x2;
		}
	} else
		return FIO_Q_COMPLETED;

	io.addr = (uint64_t)io_u->buf;
	io.data_len = io_u->buflen;

	fio_ro_check(td, io_u);

	ret = ioctl(f->fd, LNVM_PIO, &io);
	if (ret)
		printf("fail: %u\n", ret);

	if (io.result) {
		printf("result: %u\n", io.result);
		printf("status: %llu\n", (unsigned long long)io.status);
	}

	return FIO_Q_COMPLETED;
}

static int fio_lightnvm_open_file(struct thread_data *td, struct fio_file *f)
{
	struct dft_thread *th;
	int ret, nr_blks, i;

	ret = generic_open_file(td, f);
	if (ret)
		return ret;

	if (td->o.td_ddir != TD_DDIR_WRITE)
		return 0;

	nr_blks = td->o.size >> 24; /* 16MB */
	if (!nr_blks) {
		td_verror(td, -EINVAL, "size must be a multiple of 16MB");
		goto err_close;
	}

	th = &d.threads[td->subjob_number];

	th->ppas = malloc(nr_blks * sizeof(struct ppa_addr));
	if (!th->ppas) {
		td_verror(td, -ENOMEM, "not enough memory");
		goto err_close;
	}

	for (i = 0; i < nr_blks; i++) {
		struct dft_ioctl_vblk vblk;
		struct ppa_addr ppa;

		if (th->ppas[i].ppa != 0)
			continue;

		ppa.ppa = 0;
		ppa.g.ch = ((td->subjob_number) % 16);
		ppa.g.lun = (td->subjob_number) / 16;

		vblk.ppa = ppa.ppa;
		vblk.flags = 0;

		//printf("thread: %u %u ch: %u lun: %u try get\n", td->thread_number - 1, td->subjob_number, ppa.g.ch, ppa.g.lun);
		ret = ioctl(f->fd, LNVM_GET_BLOCK, &vblk);
		if (ret) {
			td_verror(td, ret, "block could not be get for vlun");
			/* clean up successful gotten blocks */
			goto err_free_ppas;
		}

		th->ppas[i].ppa = vblk.ppa;
		printf("thread: %u %u ch: %u lun: %u blk: %u got\n",
				td->thread_number - 1, td->subjob_number,
				th->ppas[i].g.ch,
				th->ppas[i].g.lun,
				th->ppas[i].g.blk);
	}

	return 0;
err_free_ppas:
	free(th->ppas);
err_close:
	{
		int fio_unused ret;
		ret = generic_close_file(td, f);
		return 1;
	}
}

static int fio_lightnvm_close_file(struct thread_data *td, struct fio_file *f)
{
	struct dft_thread *th = &d.threads[td->subjob_number];
	int ret, nr_blks, i;

	if (!td_trim(td))
		goto finish;

	nr_blks = td->o.size >> 24; /* 16MB */
	for (i = 0; i < nr_blks; i++) {
		struct dft_ioctl_vblk vblk;

		if (th->ppas[i].ppa == 0)
			continue;

		vblk.ppa = th->ppas[i].ppa;
		vblk.flags = 0;

	/*	printf("thread: %u ch: %u lun: %u blk: %u put\n",
						td->subjob_number,
						th->ppas[i].g.ch,
						th->ppas[i].g.lun,
						th->ppas[i].g.blk);*/
		ret = ioctl(f->fd, LNVM_PUT_BLOCK, &vblk);
		if (ret)
			td_verror(td, ret, "block could not be put for vlun");

		th->ppas[i].ppa = 0;
	}

finish:
	/* TODO: free th->ppas */
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
