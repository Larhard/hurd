/* 
   Copyright (C) 1994, 1997, 1999 Free Software Foundation

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

#include <fcntl.h>

#include "priv.h"
#include "io_S.h"

/* Implement io_map as described in <hurd/io.defs>. */
kern_return_t
diskfs_S_io_map (struct protid *cred,
		 memory_object_t *rdobj,
		 mach_msg_type_name_t *rdtype,
		 memory_object_t *wrobj,
		 mach_msg_type_name_t *wrtype)
{
  return diskfs_S_io_map_segment (cred, 0, rdobj, rdtype, wrobj, wrtype);
}

