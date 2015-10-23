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
#include <libaio.h>

#include "dft_ioctl.h"
#include "../fio.h"
#include "../lib/pow2.h"

static int fio_libaio_commit(struct thread_data *td);

struct fio_lightnvm_data {
	int pg_size;
	struct dft_block vblk[64];
};
struct fio_lightnvm_data d;

struct libaio_data {
	io_context_t aio_ctx;
	struct io_event *aio_events;
	struct iocb **iocbs;
	struct io_u **io_us;

	/*
	 * Basic ring buffer. 'head' is incremented in _queue(), and
	 * 'tail' is incremented in _commit(). We keep 'queued' so
	 * that we know if the ring is full or empty, when
	 * 'head' == 'tail'. 'entries' is the ring size, and
	 * 'is_pow2' is just an optimization to use AND instead of
	 * modulus to get the remainder on ring increment.
	 */
	int is_pow2;
	unsigned int entries;
	unsigned int queued;
	unsigned int head;
	unsigned int tail;
};

static inline void ring_inc(struct libaio_data *ld, unsigned int *val,
			    unsigned int add)
{
	if (ld->is_pow2)
		*val = (*val + add) & (ld->entries - 1);
	else
		*val = (*val + add) % ld->entries;
}

static int fio_libaio_prep(struct thread_data fio_unused *td, struct io_u *io_u)
{
	static int globali = 0;
	int iodepth = td->o.iodepth;
	struct fio_file *f = io_u->file;
	unsigned long long pgsize = io_u->buflen * td->o.iodepth;
	unsigned long long blkid = (io_u->offset / io_u->buflen) % td->o.iodepth;
	unsigned long long pgbase = io_u->offset / pgsize;
	struct dft_block *blk = &d.vblk[blkid];
	unsigned long long base = blk->bppa * 4096;
	unsigned long long offset = pgbase * io_u->buflen;

	if (io_u->ddir == DDIR_READ) {
//		printf("read: %lu %lu %lu %u\n", io_u->buflen, offset, base + offset, blkid);
		io_prep_pread(&io_u->iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen,
							base + offset);
	} else if (io_u->ddir == DDIR_WRITE) {
		if ((io_u->buflen % 64 * 1024) != 0) {
			io_u->error = ENOTSUP;
			td_verror(td, io_u->error,
						"size not support for backing device");
			return 0;
		}

//		printf("write: %lu %llu %lu %lu %llu %llu %llu\n", io_u->buflen, io_u->offset, offset, base + offset, blkid, pgbase, pgsize);
		io_prep_pwrite(&io_u->iocb, f->fd, io_u->xfer_buf, io_u->xfer_buflen,
							base + offset);
	}

	return 0;
}

static struct io_u *fio_libaio_event(struct thread_data *td, int event)
{
	struct libaio_data *ld = td->io_ops->data;
	struct io_event *ev;
	struct io_u *io_u;

	ev = ld->aio_events + event;
	io_u = container_of(ev->obj, struct io_u, iocb);

	if (ev->res != io_u->xfer_buflen) {
		if (ev->res > io_u->xfer_buflen) {
			printf("te\n");
			io_u->error = -ev->res;
		}
		else
			io_u->resid = io_u->xfer_buflen - ev->res;
	} else
		io_u->error = 0;

	if (io_u->error)
		printf("error: %d\n", io_u->error);
	return io_u;
}

struct aio_ring {
	unsigned id;		 /** kernel internal index number */
	unsigned nr;		 /** number of io_events */
	unsigned head;
	unsigned tail;

	unsigned magic;
	unsigned compat_features;
	unsigned incompat_features;
	unsigned header_length;	/** size of aio_ring */

	struct io_event events[0];
};

#define AIO_RING_MAGIC	0xa10a10a1

static int fio_libaio_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	struct libaio_data *ld = td->io_ops->data;
	unsigned actual_min = td->o.iodepth_batch_complete_min == 0 ? 0 : min;
	struct timespec __lt, *lt = NULL;
	int r, events = 0;

	if (t) {
		__lt = *t;
		lt = &__lt;
	}

	do {
		r = io_getevents(ld->aio_ctx, actual_min,
				max, ld->aio_events + events, lt);
		if (r > 0)
			events += r;
		else if ((min && r == 0) || r == -EAGAIN) {
			fio_libaio_commit(td);
			usleep(100);
		} else if (r != -EINTR)
			break;
	} while (events < min);

	return r < 0 ? r : events;
}

static int fio_libaio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops->data;

	fio_ro_check(td, io_u);

	if (ld->queued == td->o.iodepth)
		return FIO_Q_BUSY;

	/*
	 * fsync is tricky, since it can fail and we need to do it
	 * serialized with other io. the reason is that linux doesn't
	 * support aio fsync yet. So return busy for the case where we
	 * have pending io, to let fio complete those first.
	 */
	if (ddir_sync(io_u->ddir)) {
		if (ld->queued)
			return FIO_Q_BUSY;

		do_io_u_sync(td, io_u);
		return FIO_Q_COMPLETED;
	}

	if (io_u->ddir == DDIR_TRIM) {
		if (ld->queued)
			return FIO_Q_BUSY;

		do_io_u_trim(td, io_u);
		return FIO_Q_COMPLETED;
	}

	ld->iocbs[ld->head] = &io_u->iocb;
	ld->io_us[ld->head] = io_u;
	ring_inc(ld, &ld->head, 1);
	ld->queued++;
	return FIO_Q_QUEUED;
}

static void fio_libaio_queued(struct thread_data *td, struct io_u **io_us,
			      unsigned int nr)
{
	struct timeval now;
	unsigned int i;

	if (!fio_fill_issue_time(td))
		return;

	fio_gettime(&now, NULL);

	for (i = 0; i < nr; i++) {
		struct io_u *io_u = io_us[i];

		memcpy(&io_u->issue_time, &now, sizeof(now));
		io_u_queued(td, io_u);
	}
}

static int fio_libaio_commit(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops->data;
	struct iocb **iocbs;
	struct io_u **io_us;
	struct timeval tv;
	int ret, wait_start = 0;

	if (!ld->queued)
		return 0;

	do {
		long nr = ld->queued;

		nr = min((unsigned int) nr, ld->entries - ld->tail);
		io_us = ld->io_us + ld->tail;
		iocbs = ld->iocbs + ld->tail;

		ret = io_submit(ld->aio_ctx, nr, iocbs);
		if (ret > 0) {
			fio_libaio_queued(td, io_us, ret);
			io_u_mark_submit(td, ret);

			ld->queued -= ret;
			ring_inc(ld, &ld->tail, ret);
			ret = 0;
			wait_start = 0;
		} else if (ret == -EINTR || !ret) {
			if (!ret)
				io_u_mark_submit(td, ret);
			wait_start = 0;
			continue;
		} else if (ret == -EAGAIN) {
			/*
			 * If we get EAGAIN, we should break out without
			 * error and let the upper layer reap some
			 * events for us. If we have no queued IO, we
			 * must loop here. If we loop for more than 30s,
			 * just error out, something must be buggy in the
			 * IO path.
			 */
			if (ld->queued) {
				ret = 0;
				break;
			}
			if (!wait_start) {
				fio_gettime(&tv, NULL);
				wait_start = 1;
			} else if (mtime_since_now(&tv) > 30000) {
				log_err("fio: aio appears to be stalled, giving up\n");
				break;
			}
			usleep(1);
			continue;
		} else if (ret == -ENOMEM) {
			/*
			 * If we get -ENOMEM, reap events if we can. If
			 * we cannot, treat it as a fatal event since there's
			 * nothing we can do about it.
			 */
			if (ld->queued)
				ret = 0;
			break;
		} else
			break;
	} while (ld->queued);

	return ret;
}

static int fio_libaio_cancel(struct thread_data *td, struct io_u *io_u)
{
	struct libaio_data *ld = td->io_ops->data;

	return io_cancel(ld->aio_ctx, &io_u->iocb, ld->aio_events);
}

static void fio_libaio_cleanup(struct thread_data *td)
{
	struct libaio_data *ld = td->io_ops->data;

	if (ld) {
		/*
		 * Work-around to avoid huge RCU stalls at exit time. If we
		 * don't do this here, then it'll be torn down by exit_aio().
		 * But for that case we can parallellize the freeing, thus
		 * speeding it up a lot.
		 */
		if (!(td->flags & TD_F_CHILD))
			io_destroy(ld->aio_ctx);
		free(ld->aio_events);
		free(ld->iocbs);
		free(ld->io_us);
		free(ld);
	}
}

static int fio_libaio_init(struct thread_data *td)
{
	struct libaio_data *ld;
	int err = 0;

	ld = calloc(1, sizeof(*ld));

	/*
	 * First try passing in 0 for queue depth, since we don't
	 * care about the user ring. If that fails, the kernel is too old
	 * and we need the right depth.
	 */
	err = io_queue_init(256, &ld->aio_ctx);
	if (err) {
		td_verror(td, -err, "io_queue_init");
		log_err("fio: check /proc/sys/fs/aio-max-nr\n");
		free(ld);
		return 1;
	}

	ld->entries = td->o.iodepth;
	ld->is_pow2 = is_power_of_2(ld->entries);
	ld->aio_events = calloc(ld->entries, sizeof(struct io_event));
	ld->iocbs = calloc(ld->entries, sizeof(struct iocb *));
	ld->io_us = calloc(ld->entries, sizeof(struct io_u *));

	td->io_ops->data = ld;
	return 0;
}

static int fio_lightnvm_open_file(struct thread_data *td, struct fio_file *f)
{
	int iodepth = td->o.iodepth;
	struct dft_block *blk;
	int ret, i;
	int chnl_id;
	int lun_id;

	ret = generic_open_file(td, f);
	if (ret)
		return ret;

	for (i = 0; i < iodepth; i++) {
		blk = &d.vblk[i];

		if (blk->id == 0 && td->o.td_ddir == TD_DDIR_WRITE) {
			chnl_id = ((i) % 16) * 4;
			lun_id = (i) / 16;

			blk->vlun_id = chnl_id + lun_id;

			ret = ioctl(f->fd, LNVM_GET_BLOCK, blk);
			if (ret) {
				td_verror(td, ret, "block could not be get for vlun");
				goto err_close;
			}
			printf("iodepth: %u vlun: %u blk %lu get\n", iodepth, blk->vlun_id, blk->id);
		}
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
	int iodepth = td->o.iodepth;
	int ret, i;
	struct dft_block *blk;

	for (i = 0; i < iodepth; i++) {
		blk = &d.vblk[i];

		if (blk->id != 0 && td_trim(td)) {
			printf("iodepth: %u blk %lu put\n", i, blk->id);
			ret = ioctl(f->fd, LNVM_PUT_BLOCK, blk);
			if (ret)
				td_verror(td, ret, "block could not be get for vlun");

			blk->id = 0;
		}
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
	.open_file	= fio_lightnvm_open_file,
	.close_file	= fio_lightnvm_close_file,
	.get_file_size	= fio_lightnvm_get_file_size,

	/* libaio integration */
	.queue			= fio_libaio_queue,
	.init			= fio_libaio_init,
	.prep			= fio_libaio_prep,
	.commit			= fio_libaio_commit,
	.cancel			= fio_libaio_cancel,
	.getevents		= fio_libaio_getevents,
	.event			= fio_libaio_event,
	.cleanup		= fio_libaio_cleanup,

	.flags		= FIO_MEMALIGN,
};

static void fio_init fio_lightnvm_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_lightnvm_unregister(void)
{
	unregister_ioengine(&ioengine);
}
