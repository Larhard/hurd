#ifndef _OSS_LOGGING_H
#define _OSS_LOGGING_H

#include <stdio.h>
#include <unistd.h>

#include "config.h"

extern FILE *oss_log;

#define init_logging() (oss_log = fopen(oss_log, "at"))
#define close_logging() fclose(oss_log)

#define write_log(level,fmt,args...) \
	fprintf(oss_log, "%s: oss[%d]: " fmt "\n", level, getpid(), ##args)

#define err(fmt,args...) write_log("ERROR", fmt, ##args)
#define warn(fmt,args...) write_log("WARNING", fmt, ##args)
#define info(fmt,args...) write_log("INFO", fmt, ##args)

#ifdef NDEBUG
#define debug(fmt,args...)
#else /* DEBUG */
#define debug(fmt,args...) write_log("DEBUG", fmt, ##args)
#endif /* DEBUG */


#endif /* _OSS_LOGGING_H */
