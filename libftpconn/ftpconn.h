/* Manage an ftp connection

   Copyright (C) 1997 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#ifndef __FTPCONN_H__
#define __FTPCONN_H__

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

struct ftp_conn;
struct ftp_conn_params;
struct ftp_conn_stat;

/* The type of the function called by ...get_stats to add each new stat.
   NAME is the file in question, STAT is stat info about it, and if NAME is a
   symlink, SYMLINK_TARGET is what it is linked to, or 0 if it's not a symlink.
   NAME is malloced and should be freed by the callee if it's not saved.
   HOOK is as passed into ...get_stats.  */
typedef error_t (*ftp_conn_add_stat_fun_t) (char *name, struct stat *stat,
					    char *symlink_target,
					    void *hook);

/* Hooks that customize behavior for particular types of remote system.  */
struct ftp_conn_syshooks
{
  /* Should return in ADDR a malloced struct sockaddr containing the address
     of the host referenced by the PASV reply contained in TXT.  */
  error_t (*pasv_addr) (struct ftp_conn *conn, const char *txt,
			struct sockaddr **addr);
  
  /* Look at the error string in TXT, and try to guess an error code to
     return.  If POSS_ERRS is non-zero, it contains a list of errors
     that are likely to occur with the previous command, terminated with 0.
     If no match is found and POSS_ERRS is non-zero, the first error in
     POSS_ERRS should be returned by default.  */
  error_t (*interp_err) (struct ftp_conn *conn, const char *txt,
			 const error_t *poss_errs);

  /* Start an operation to get a list of file-stat structures for NAME (this
     is often similar to ftp_conn_start_dir, but with OS-specific flags), and
     return a file-descriptor for reading on, and a state structure in STATE
     suitable for passing to cont_get_stats.  FORCE_DIR controls what happens
     if NAME refers to a directory: if FORCE_DIR is false, STATS will contain
     entries for all files *in* NAME, and if FORCE_DIR is true, it will
     contain just a single entry for NAME itself (or an error will be
     returned when this isn't possible).  */
  error_t (*start_get_stats) (struct ftp_conn *conn, const char *name,
			      int force_dir,
			      int *fd, void **state);

  /* Read stats information from FD, calling ADD_STAT for each new stat (HOOK
     is passed to ADD_STAT).  FD and STATE should be returned from
     start_get_stats.  If this function returns EAGAIN, then it should be
     called again to finish the job (possibly after calling select on FD); if
     it returns 0, then it is finishe,d and FD and STATE are deallocated.  */
  error_t (*cont_get_stats) (struct ftp_conn *conn, int fd, void *state,
			     ftp_conn_add_stat_fun_t add_stat, void *hook);
};

/* Type parameter for the cntl_debug hook.  */
#define FTP_CONN_CNTL_DEBUG_CMD		1
#define FTP_CONN_CNTL_DEBUG_REPLY	2

/* Type parameter for the get_login_param hook.  */
#define FTP_CONN_GET_LOGIN_PARAM_USER	1
#define FTP_CONN_GET_LOGIN_PARAM_PASS	2
#define FTP_CONN_GET_LOGIN_PARAM_ACCT	3

/* General connection customization.  */
struct ftp_conn_hooks
{
  /* If non-zero, should look at the SYST reply in SYST, and fill in CONN's
     syshooks (with ftp_conn_set_hooks) appropriately; SYST may be zero if
     the remote system doesn't support that command.  If zero, then the
     default ftp_conn_choose_syshooks is used.  */
  void (*choose_syshooks) (struct ftp_conn *conn, const char *syst);

  /* If non-zero, called during io on the ftp control connection -- TYPE is
     FTP_CONN_CNTL_DEBUG_CMD for commands, and FTP_CONN_CNTL_DEBUG_REPLY for
     replies; TXT is the actual text.  */
  void (*cntl_debug) (struct ftp_conn *conn, int type, const char *txt);

  /* Called after CONN's connection the server has been opened (or reopened).  */
  void (*opened) (struct ftp_conn *conn);

  /* If the remote system requires some login parameter that isn't available,
     this hook is called to try and get it, returning a value in TXT.  The
     return value should be in a malloced block of memory.  The returned
     value will only be used once; if it's desired that it should `stick',
     the user may modify the value stored in CONN's params field, but that is
     an issue outside of the scope of this interface -- params are only read,
     never written.  */
  error_t (*get_login_param) (struct ftp_conn *conn, int type, char **txt);

  /* Called after CONN's connection the server has closed for some reason.  */
  void (*closed) (struct ftp_conn *conn);

  /* Called when CONN is initially created before any other hook calls.  An
     error return causes the creation to fail with that error code.  */
  error_t (*init) (struct ftp_conn *conn);

  /* Called when CONN is about to be destroyed.  No hook calls are ever made
     after this one.  */
  void (*fini) (struct ftp_conn *conn);
};

/* A single ftp connection.  */
struct ftp_conn
{
  const struct ftp_conn_params *params;	/* machine, user, &c */
  const struct ftp_conn_hooks *hooks; /* Customization hooks. */

  struct ftp_conn_syshooks syshooks; /* host-dependent hook functions */

  int control;			/* fd for ftp control connection */

  char *line;			/* buffer for reading control replies */
  size_t line_sz;		/* allocated size of LINE */
  size_t line_offs;		/* Start of unread input in LINE.  */
  size_t line_len;		/* End of the contents in LINE.  */

  char *reply_txt;		/* A buffer for the text of entire replies */
  size_t reply_txt_sz;		/* size of it */

  char *cwd;			/* Last know CWD, or 0 if unknown.  */
  const char *type;		/* Connection type, or 0 if default.  */

  void *hook;			/* Random user data. */
};

/* Parameters for an ftp connection; doesn't include any actual connection
   state.  */
struct ftp_conn_params
{
  void *addr;			/* Address.  */
  size_t addr_len;		/* Length in bytes of ADDR.  */
  int addr_type;		/* Type of ADDR (AF_*).  */

  char *user, *pass, *acct;	/* Parameters for logging into ftp.  */
};

/* Unix hooks */
extern error_t ftp_conn_unix_pasv_addr (struct ftp_conn *conn, const char *txt,
					struct sockaddr **addr);
extern error_t ftp_conn_unix_interp_err (struct ftp_conn *conn, const char *txt,
					 error_t *poss_errs);
extern error_t ftp_conn_unix_start_get_stats (struct ftp_conn *conn,
					      const char *name,
					      int force_dir, int *fd,
					      void **state);
extern error_t ftp_conn_unix_cont_get_stats (struct ftp_conn *conn,
					     int fd, void *state,
					     ftp_conn_add_stat_fun_t add_stat,
					     void *hook);

extern struct ftp_conn_syshooks ftp_conn_unix_syshooks;

error_t
ftp_conn_get_reply (struct ftp_conn *conn, int *reply, const char **reply_txt);

error_t
ftp_conn_cmd (struct ftp_conn *conn, const char *cmd, const char *arg,
	       int *reply, const char **reply_txt);

error_t
ftp_conn_cmd_reopen (struct ftp_conn *conn, const char *cmd, const char *arg,
		      int *reply, const char **reply_txt);

void ftp_conn_abort (struct ftp_conn *conn);

/* Sets CONN's syshooks to a copy of SYSHOOKS.  */
void ftp_conn_set_syshooks (struct ftp_conn *conn,
			    struct ftp_conn_syshooks *syshooks);

error_t ftp_conn_open (struct ftp_conn *conn);

void ftp_conn_close (struct ftp_conn *conn);

error_t ftp_conn_create (const struct ftp_conn_params *params,
			 const struct ftp_conn_hooks *hooks,
			 struct ftp_conn **conn);

void ftp_conn_free (struct ftp_conn *conn);

/* Start a transfer command CMD (and optional args ...), returning a file
   descriptor in DATA.  POSS_ERRS is a list of errnos to try matching
   against any resulting error text.  */
error_t
ftp_conn_start_transfer (struct ftp_conn *conn,
			 const char *cmd, const char *arg,
			 error_t *poss_errs,
			 int *data);

/* Wait for the reply signalling the end of a data transfer.  */
error_t ftp_conn_finish_transfer (struct ftp_conn *conn);

/* Start retreiving file NAME over CONN, returning a file descriptor in DATA
   over which the data can be read.  */
error_t ftp_conn_start_retrieve (struct ftp_conn *conn, const char *name, int *data);

/* Start retreiving a list of files in NAME over CONN, returning a file
   descriptor in DATA over which the data can be read.  */
error_t ftp_conn_start_list (struct ftp_conn *conn, const char *name, int *data);

/* Start retreiving a directory listing of NAME over CONN, returning a file
   descriptor in DATA over which the data can be read.  */
error_t ftp_conn_start_dir (struct ftp_conn *conn, const char *name, int *data);

/* Start storing into file NAME over CONN, returning a file descriptor in DATA
   into which the data can be written.  */
error_t ftp_conn_start_store (struct ftp_conn *conn, const char *name, int *data);

/* Transfer the output of SRC_CMD/SRC_NAME on SRC_CONN to DST_NAME on
   DST_CONN, moving the data directly between servers.  */
error_t
ftp_conn_rmt_transfer (struct ftp_conn *src_conn,
		       const char *src_cmd, const char *src_name,
		       const int *src_poss_errs,
		       struct ftp_conn *dst_conn, const char *dst_name);

/* Copy the SRC_NAME on SRC_CONN to DST_NAME on DST_CONN, moving the data
   directly between servers.  */
error_t
ftp_conn_rmt_copy (struct ftp_conn *src_conn, const char *src_name,
		   struct ftp_conn *dst_conn, const char *dst_name);

/* Return a malloced string containing CONN's working directory in CWD.  */
error_t ftp_conn_get_cwd (struct ftp_conn *conn, char **cwd);

/* Return a malloced string containing CONN's working directory in CWD.  */
error_t ftp_conn_cwd (struct ftp_conn *conn, const char *cwd);

/* Return a malloced string containing CONN's working directory in CWD.  */
error_t ftp_conn_cdup (struct ftp_conn *conn);

/* Set the ftp connection type of CONN to TYPE, or return an error.  */
error_t ftp_conn_set_type (struct ftp_conn *conn, const char *type);

/* Start an operation to get a list of file-stat structures for NAME (this
   is often similar to ftp_conn_start_dir, but with OS-specific flags), and
   return a file-descriptor for reading on, and a state structure in STATE
   suitable for passing to cont_get_stats.  FORCE_DIR controls what happens if
   NAME refers to a directory: if FORCE_DIR is false, STATS will contain
   entries for all files *in* NAME, and if FORCE_DIR is true, it will
   contain just a single entry for NAME itself (or an error will be
   returned when this isn't possible).  */
error_t ftp_conn_start_get_stats (struct ftp_conn *conn,
				  const char *name, int force_dir,
				  int *fd, void **state);

/* Read stats information from FD, calling ADD_STAT for each new stat (HOOK
   is passed to ADD_STAT).  FD and STATE should be returned from
   start_get_stats.  If this function returns EAGAIN, then it should be
   called again to finish the job (possibly after calling select on FD); if
   it returns 0, then it is finishe,d and FD and STATE are deallocated.  */
error_t ftp_conn_cont_get_stats (struct ftp_conn *conn, int fd, void *state,
				 ftp_conn_add_stat_fun_t add_stat, void *hook);

/* Get a list of file-stat structures for NAME, calling ADD_STAT for each one
   (HOOK is passed to ADD_STAT).  If NAME refers to an ordinary file, a
   single entry for it is returned for it; if NAME refers to a directory,
   then if FORCE_DIR is false, STATS will contain entries for all files *in*
   NAME, and if FORCE_DIR is true, it will contain just a single entry for
   NAME itself (or an error will be returned when this isn't possible).  This
   function may block.  */
error_t ftp_conn_get_stats (struct ftp_conn *conn,
			    const char *name, int force_dir,
			    ftp_conn_add_stat_fun_t add_stat, void *hook);

#endif /* __FTPCONN_H__ */