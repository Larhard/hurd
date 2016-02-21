#define IO_INTRAN trivfs_protid_t trivfs_begin_using_protid (io_t)
#define IO_INTRAN_PAYLOAD trivfs_protid_t trivfs_begin_using_protid_payload
#define IO_DESTRUCTOR trivfs_end_using_protid (trivfs_protid_t)

#define TIOCTL_IMPORTS import "libtrivfs/mig-decls.h";
