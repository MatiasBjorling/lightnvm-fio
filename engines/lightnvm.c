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
	struct dft_block *blk;
	unsigned long long base;
	int ret;

	blk = &d.vblk[td->subjob_number];
	if (td->groupid == 1 && td->o.td_ddir == TD_DDIR_WRITE)
		blk = &d.vblk[td->subjob_number+8];

	fio_ro_check(td, io_u);
	base = blk->bppa * 4096;

	if (io_u->ddir == DDIR_READ){
//		printf("read: %lu %lu %u\n", io_u->buflen, io_u->offset, td->thread_number);
		ret = pread(f->fd, io_u->buf, io_u->buflen,
							base + io_u->offset);
	} else if (io_u->ddir == DDIR_WRITE) {
		if ((io_u->buflen % 64 * 1024) != 0) {
			io_u->error = ENOTSUP;
			td_verror(td, io_u->error,
						"size not support for backing device");
			return 0;
		}

//		printf("write: %lu %lu %u\n", io_u->buflen, io_u->offset, td->thread_number);
		ret = pwrite(f->fd, io_u->buf, io_u->buflen,
							base + io_u->offset);
		if (ret < 0)
			printf("write failed\n");
	}

	return FIO_Q_COMPLETED;
}

static int fio_lightnvm_open_file(struct thread_data *td, struct fio_file *f)
{
	struct dft_block *blk;
	int ret;
	int chnl_id;
	int lun_id;
	int job_id;

	ret = generic_open_file(td, f);
	if (ret)
		return ret;

	printf("groupid: %u subjob: %u\n", td->groupid, td->subjob_number);

	if (td->groupid == 0)
		job_id = td->subjob_number;
	else if (td->groupid == 1)
		job_id = td->subjob_number + 8;
	else
		return 0;

	blk = &d.vblk[job_id];
	if (blk->id == 0 && td->o.td_ddir == TD_DDIR_WRITE) {
		chnl_id = ((job_id) % 16) * 4;
		lun_id = (job_id) / 16;

		blk->vlun_id = chnl_id + lun_id;

		ret = ioctl(f->fd, LNVM_GET_BLOCK, blk);
		if (ret) {
			td_verror(td, ret, "block could not be get for vlun");
			goto err_close;
		}
		printf("job id: %u vlun: %u blk %lu get\n", job_id, blk->vlun_id, blk->id);
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

	if (blk->id != 0 && td_trim(td)) {
		printf("thread: %u blk %lu put\n", td->subjob_number,
								blk->id);
		ret = ioctl(f->fd, LNVM_PUT_BLOCK, blk);
		if (ret)
			td_verror(td, ret, "block could not be get for vlun");

		blk->id = 0;
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
