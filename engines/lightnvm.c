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
	//struct dft_block *blk = &d.vblk[td->subjob_number];
	struct dft_ioctl_io io;
	int ret;
	struct ppa_addr ppa;

	memset(&io, 0, sizeof(io));
	ppa.ppa = 0;
	ppa.g.blk = 1;

	io.opcode = 0;
	io.flags = 0;
	io.nppas = 1;
	io.ppas = ppa.ppa;
	io.addr = (uint64_t)io_u->buf;
	io.data_len = io_u->buflen;

	fio_ro_check(td, io_u);

	ret = ioctl(f->fd, LNVM_PIO, &io);
	if (ret)
		printf("fail: %u\n", ret);
/*	if (io_u->ddir == DDIR_READ) {
	} else if (io_u->ddir == DDIR_WRITE) {
	}*/

	return FIO_Q_COMPLETED;
}

static int fio_lightnvm_open_file(struct thread_data *td, struct fio_file *f)
{
	//struct dft_block *blk;
	int ret;
	/*int chnl_id;
	int lun_id;*/

	ret = generic_open_file(td, f);
	if (ret)
		return ret;

/*	blk = &d.vblk[td->subjob_number];

	if (blk->id == 0 && td->o.td_ddir == TD_DDIR_WRITE) {
		chnl_id = ((td->subjob_number) % 16) * 4;
		lun_id = (td->subjob_number) / 16;

		blk->vlun_id = chnl_id + lun_id;

		ret = ioctl(f->fd, LNVM_GET_BLOCK, blk);
		if (ret) {
			td_verror(td, ret, "block could not be get for vlun");
			goto err_close;
		}
		printf("thread: %u %u vlun: %u blk %lu get\n", td->thread_number - 1, td->subjob_number, blk->vlun_id, blk->id);
	}
*/
	return 0;

/*err_close:
	{
		int fio_unused ret;
		ret = generic_close_file(td, f);
		return 1;
	}*/
}

static int fio_lightnvm_close_file(struct thread_data *td, struct fio_file *f)
{
	//struct dft_block *blk = &d.vblk[td->subjob_number];
//	int ret;

/*	if (blk->id != 0 && td_trim(td)) {
		printf("thread: %u blk %lu put\n", td->subjob_number,
								blk->id);
		ret = ioctl(f->fd, LNVM_PUT_BLOCK, blk);
		if (ret)
			td_verror(td, ret, "block could not be get for vlun");

		blk->id = 0;
	}
	*/

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
