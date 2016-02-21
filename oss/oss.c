#define _GNU_SOURCE 1

#include <argp.h>
#include <error.h>
#include <fcntl.h>
#include <hurd.h>
#include <hurd/trivfs.h>
#include <stdio.h>
#include <sys/mman.h>

#include "tioctl_S.h"

#include "config.h"
#include "logging.h"

/* peropen data */
struct peropen_data
{
};

/* trivfs hooks */
int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;

int trivfs_allow_open = O_WRITE;

int trivfs_support_read = 0;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

void
trivfs_modify_stat(struct trivfs_protid *cred, io_statbuf_t *stbuf)
{
	stbuf->st_mode &= ~S_IFMT;
	stbuf->st_mode |= S_IFCHR;
	stbuf->st_size = 0;
}

kern_return_t
trivfs_goaway(struct trivfs_control *cntl, int flags)
{
	info("going away");
	exit(0);
}

/* open trivfs */
kern_return_t
open_hook(struct trivfs_peropen *peropen)
{
	struct peropen_data *op = malloc(sizeof(struct peropen_data));
	if (op == NULL) {
		return errno;
	}

	peropen->hook = op;

	return 0;
}

error_t (*trivfs_peropen_create_hook) (struct trivfs_peropen *perop) = open_hook;

/* close trivfs */
void
close_hook(struct trivfs_peropen *peropen)
{
	free(peropen->hook);
}

void (*trivfs_peropen_destroy_hook) (struct trivfs_peropen *perop) = close_hook;

/* read from trivfs */
kern_return_t
trivfs_S_io_read (struct trivfs_protid *cred,
		mach_port_t reply, mach_msg_type_name_t reply_type,
		vm_address_t *data, mach_msg_type_number_t *data_len,
		off_t offs, mach_msg_type_number_t amount)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else if (! (cred->po->openmodes & O_READ)) {
		return EBADF;
	}

	if (amount > 0) {
		int i;

		if (*data_len < amount) {
			*data = (vm_address_t) mmap (0, amount, PROT_READ|PROT_WRITE,
					MAP_ANON, 0, 0);
		}

		for (i = 0; i < amount; i++) {
			((char *) *data)[i] = 97;
		}
	}

	*data_len = amount;
	return 0;
}

/* write to trivfs */
kern_return_t
trivfs_S_io_write (struct trivfs_protid *cred,
		mach_port_t reply, mach_msg_type_name_t reply_type,
		vm_address_t data, mach_msg_type_number_t data_len,
		off_t offs, mach_msg_type_number_t *amount)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else if (! (cred->po->openmodes & O_WRITE)) {
		return EBADF;
	}

	*amount = data_len;
	return 0;
}

/* Tell how much data can be read from the object without blocking for
   a "long time" (this should be the same meaning of "long time" used
   by the nonblocking flag.  */
kern_return_t
trivfs_S_io_readable (struct trivfs_protid *cred,
		mach_port_t reply, mach_msg_type_name_t replytype,
		mach_msg_type_number_t *amount)
{
	if (!cred)
		return EOPNOTSUPP;
	else if (!(cred->po->openmodes & O_READ))
		return EINVAL;
	else
		*amount = 0;
	return 0;
}

/* Truncate file.  */
kern_return_t
trivfs_S_file_set_size (struct trivfs_protid *cred, off_t size)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else {
		return 0;
	}
}

/* Change current read/write offset */
kern_return_t
trivfs_S_io_seek (struct trivfs_protid *cred, mach_port_t reply,
		mach_msg_type_name_t reply_type, off_t offs, int whence,
		off_t *new_offs)
{
	if (! cred) {
		return EOPNOTSUPP;
	} else {
		return 0;
	}
}

/* SELECT_TYPE is the bitwise OR of SELECT_READ, SELECT_WRITE, and
   SELECT_URG.  Block until one of the indicated types of i/o can be
   done "quickly", and return the types that are then available.
   TAG is returned as passed; it is just for the convenience of the
   user in matching up reply messages with specific requests sent.  */
kern_return_t
trivfs_S_io_select (struct trivfs_protid *cred,
		mach_port_t reply, mach_msg_type_name_t replytype,
		int *type, int *tag)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else {
		if (((*type & SELECT_READ) && !(cred->po->openmodes & O_READ))
				|| ((*type & SELECT_WRITE) && !(cred->po->openmodes & O_WRITE))) {
			return EBADF;
		} else {
			*type &= ~SELECT_URG;
		}
	}

	return 0;
}

/* Well, we have to define these four functions, so here we go: */
kern_return_t
trivfs_S_io_get_openmodes (struct trivfs_protid *cred, mach_port_t reply,
		mach_msg_type_name_t replytype, int *bits)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else {
		*bits = cred->po->openmodes;
		return 0;
	}
}

kern_return_t
trivfs_S_io_set_all_openmodes (struct trivfs_protid *cred,
		mach_port_t reply,
		mach_msg_type_name_t replytype,
		int mode)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else {
		return 0;
	}
}

kern_return_t
trivfs_S_io_set_some_openmodes (struct trivfs_protid *cred,
		mach_port_t reply,
		mach_msg_type_name_t replytype,
		int bits)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else {
		return 0;
	}
}

kern_return_t
trivfs_S_io_clear_some_openmodes (struct trivfs_protid *cred,
		mach_port_t reply,
		mach_msg_type_name_t replytype,
		int bits)
{
	if (!cred) {
		return EOPNOTSUPP;
	} else {
		return 0;
	}
}

/* ioctls */

/* SNDCTL_DSP_BIND_CHANNEL */
kern_return_t
S_tioctl_sndctl_dsp_bind_channel (trivfs_protid_t port, int *binding)
{
	*binding = 12345;
	return 0;
}

/* demuxer */
int
oss_demuxer (mach_msg_header_t *inp,
             mach_msg_header_t *outp)
{
	extern int trivfs_demuxer (mach_msg_header_t *inp, mach_msg_header_t *outp);
	extern int tioctl_server (mach_msg_header_t *inp, mach_msg_header_t *outp);

	return (trivfs_demuxer (inp, outp)
			|| tioctl_server (inp, outp));
}

/* process arguments */
static const struct argp argp = {
	.doc = "Translator for OSS"
};

/* main */
int
main (int argc, char *argv[])
{
	init_logging();

	error_t err;
	mach_port_t bootstrap;
	struct trivfs_control *fsys;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err) {
		error(1, err, "argp_parse");
	}

	task_get_bootstrap_port(mach_task_self(), &bootstrap);
	if (bootstrap == MACH_PORT_NULL) {
		error(1, 0, "must be started as translator");
	}

	/* reply to our parent */
	err = trivfs_startup(bootstrap, 0, NULL, NULL, NULL, NULL, &fsys);
	if (err) {
		error(3, err, "trivfs_startup");
	}

	/* launch */
	info("start oss translator");
	ports_manage_port_operations_one_thread(fsys->pi.bucket, oss_demuxer, 0);

	return 0;
}
