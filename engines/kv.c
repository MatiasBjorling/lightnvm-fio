/*
 * kv engine
 *
 * IO engine that utilizes the key-value IOCTL for submitting KV specific requests.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "../fio.h"

enum vsl_kv_opcode
{
	VSL_KV_GET	= 0x00,
	VSL_KV_PUT	= 0x01,
	VSL_KV_UPDATE	= 0x02,
	VSL_KV_DEL	= 0x03,
};

struct __attribute__((packed)) vsl_kv_cmd
{
	uint8_t		opcode;
	uint8_t		res[3];
	uint16_t	key_len;
	uint16_t	val_len;
	uint64_t	key_addr;
	uint64_t	val_addr;
};

#define VSL_IOC_MAGIC 'O'
#define VSL_IOCTL_ID		_IO(VSL_IOC_MAGIC, 0x40)
#define VSL_IOCTL_KV		_IOWR(VSL_IOC_MAGIC, 0x50, struct vsl_kv_cmd)

enum vsl_error {
	VSLKV_ERR_OPEN = 1,
	VSLKV_ERR_IOCTL,
};

struct kvio_data {
	struct vsl_kv_cmd cmd;
	void *key;
	void *value;
};

static int fio_kv_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct kvio_data *kd = td->io_ops->data;
	int ret;

	ret = ioctl(f->fd, VSL_IOCTL_KV, kd->cmd);
	if (ret < 0)
		return ret;

	return FIO_Q_COMPLETED;
}

static int fio_kv_init(struct thread_data *td)
{
	struct kvio_data *kd;

	kd = malloc(sizeof(struct kvio_data));

	kd->key = malloc(sizeof(char) * 13);
	sprintf(kd->key, "LIGHTNVM FTW");

	kd->value = malloc(sizeof(char) * 4096);
	memset(kd->value, 0xFF, 4096);

	kd->cmd.opcode = VSL_KV_GET;
	kd->cmd.key_len = 12;
	kd->cmd.key_addr = (uint64_t) kd->key;
	kd->cmd.val_len = 4096;
	kd->cmd.val_addr = (uint64_t) kd->value;

	td->io_ops->data = kd;
	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "kv",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_kv_init,
	.queue		= fio_kv_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.get_file_size	= generic_get_file_size,
	.flags		= FIO_SYNCIO,
};

static void fio_init fio_kv_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_kv_unregister(void)
{
	unregister_ioengine(&ioengine);
}
