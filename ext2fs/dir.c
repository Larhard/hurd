/* Directory management routines

   Copyright (C) 1994, 1995, 1996, 1997, 1998 Free Software Foundation, Inc.

   Converted for ext2fs by Miles Bader <miles@gnu.ai.mit.edu>

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

#include "ext2fs.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <stddef.h>

/* This isn't quite right because a file system block may straddle several
   device blocks, and so a write failure between writing two device blocks
   may scramble things up a bit.  But the linux doesn't do this.  We could
   try and make sure that we never wrote any modified directories with
   entries that straddle device blocks (but read those that do)...  */
#define DIRBLKSIZ block_size

enum slot_status
{
  /* This means we haven't yet found room for a new entry.  */
  LOOKING,

  /* This means that the specified entry is free and should be used. */
  TAKE,

  /* This means that the specified entry has enough room at the end
     to hold the new entry. */
  SHRINK,

  /* This means that there is enough space in the block, but not in
     any one single entry, so they all have to be shifted to make
     room.  */
  COMPRESS,

  /* This means that the directory will have to be grown to hold the
     entry. */
  EXTEND,

  /* For removal and rename, this means that this is the location
     of the entry found.  */
  HERE_TIS,
};

struct dirstat
{
  /* Type of followp operation expected */
  enum lookup_type type;

  /* One of the statuses above */
  enum slot_status stat;

  /* Mapped address and length of directory */
  vm_address_t mapbuf;
  vm_size_t mapextent;

  /* Index of this directory block. */
  int idx;

  /* For stat COMPRESS, this is the address (inside mapbuf)
     of the first direct in the directory block to be compressed. */
  /* For stat HERE_TIS, SHRINK, and TAKE, this is the entry referenced. */
  struct ext2_dir_entry *entry;

  /* For stat HERE_TIS, type REMOVE, this is the address of the immediately
     previous direct in this directory block, or zero if this is the first. */
  struct ext2_dir_entry *preventry;

  /* For stat COMPRESS, this is the number of bytes needed to be copied
     in order to undertake the compression. */
  size_t nbytes;
};

size_t diskfs_dirstat_size = sizeof (struct dirstat);

/* Initialize DS such that diskfs_drop_dirstat will ignore it. */
void
diskfs_null_dirstat (struct dirstat *ds)
{
  ds->type = LOOKUP;
}

static error_t
dirscanblock (vm_address_t blockoff, struct node *dp, int idx,
	      const char *name, int namelen, enum lookup_type type,
	      struct dirstat *ds, ino_t *inum);

/* Implement the diskfs_lookup from the diskfs library.  See
   <hurd/diskfs.h> for the interface specification.  */
error_t
diskfs_lookup_hard (struct node *dp, const char *name, enum lookup_type type,
		    struct node **npp, struct dirstat *ds, struct protid *cred)
{
  error_t err;
  ino_t inum;
  int namelen;
  int spec_dotdot;
  struct node *np = 0;
  int retry_dotdot = 0;
  vm_prot_t prot =
    (type == LOOKUP) ? VM_PROT_READ : (VM_PROT_READ | VM_PROT_WRITE);
  memory_object_t memobj;
  vm_address_t buf = 0;
  vm_size_t buflen = 0;
  int blockaddr;
  int idx, lastidx;
  int looped;

  if ((type == REMOVE) || (type == RENAME))
    assert (npp);

  if (npp)
    *npp = 0;

  spec_dotdot = type & SPEC_DOTDOT;
  type &= ~SPEC_DOTDOT;

  namelen = strlen (name);

  if (namelen > EXT2_NAME_LEN)
    return ENAMETOOLONG;

 try_again:
  if (ds)
    {
      ds->type = LOOKUP;
      ds->mapbuf = 0;
      ds->mapextent = 0;
    }
  if (buf)
    {
      vm_deallocate (mach_task_self (), buf, buflen);
      buf = 0;
    }
  if (ds && (type == CREATE || type == RENAME))
    ds->stat = LOOKING;

  /* Map in the directory contents. */
  memobj = diskfs_get_filemap (dp, prot);

  if (memobj == MACH_PORT_NULL)
    return errno;

  buf = 0;
  /* We allow extra space in case we have to do an EXTEND. */
  buflen = round_page (dp->dn_stat.st_size + DIRBLKSIZ);
  err = vm_map (mach_task_self (),
		&buf, buflen, 0, 1, memobj, 0, 0, prot, prot, 0);
  mach_port_deallocate (mach_task_self (), memobj);

  inum = 0;

  if (!diskfs_check_readonly ())
    dp->dn_set_atime = 1;

  /* Start the lookup at DP->dn->dir_idx.  */
  idx = dp->dn->dir_idx;
  if (idx * DIRBLKSIZ > dp->dn_stat.st_size)
    idx = 0;			/* just in case */
  blockaddr = buf + idx * DIRBLKSIZ;
  looped = (idx == 0);
  lastidx = idx;
  if (lastidx == 0)
    lastidx = dp->dn_stat.st_size / DIRBLKSIZ;

  while (!looped || idx < lastidx)
    {
      err = dirscanblock (blockaddr, dp, idx, name, namelen, type, ds, &inum);
      if (!err)
	{
	  dp->dn->dir_idx = idx;
	  break;
	}
      if (err != ENOENT)
	{
	  vm_deallocate (mach_task_self (), buf, buflen);
	  return err;
	}

      blockaddr += DIRBLKSIZ;
      idx++;
      if (blockaddr - buf >= dp->dn_stat.st_size && !looped)
	{
	  /* We've gotten to the end; start back at the beginning */
	  looped = 1;
	  blockaddr = buf;
	  idx = 0;
	}
    }

  if (!diskfs_check_readonly ())
    dp->dn_set_atime = 1;
  if (diskfs_synchronous)
    diskfs_node_update (dp, 1);

  /* If err is set here, it's ENOENT, and we don't want to
     think about that as an error yet. */
  err = 0;

  if (inum && npp)
    {
      if (namelen != 2 || name[0] != '.' || name[1] != '.')
	{
	  if (inum == dp->cache_id)
	    {
	      np = dp;
	      diskfs_nref (np);
	    }
	  else
	    {
	      err = diskfs_cached_lookup (inum, &np);
	      if (err)
		goto out;
	    }
	}

      /* We are looking up .. */
      /* Check to see if this is the root of the filesystem. */
      else if (dp->cache_id == 2)
	{
	  err = EAGAIN;
	  goto out;
	}

      /* We can't just do diskfs_cached_lookup, because we would then deadlock.
	 So we do this.  Ick.  */
      else if (retry_dotdot)
	{
	  /* Check to see that we got the same answer as last time. */
	  if (inum != retry_dotdot)
	    {
	      /* Drop what we *thought* was .. (but isn't any more) and
		 try *again*. */
	      diskfs_nput (np);
	      mutex_unlock (&dp->lock);
	      err = diskfs_cached_lookup (inum, &np);
	      mutex_lock (&dp->lock);
	      if (err)
		goto out;
	      retry_dotdot = inum;
	      goto try_again;
	    }
	  /* Otherwise, we got it fine and np is already set properly. */
	}
      else if (!spec_dotdot)
	{
	  /* Lock them in the proper order, and then
	     repeat the directory scan to see if this is still
	     right.  */
	  mutex_unlock (&dp->lock);
	  err = diskfs_cached_lookup (inum, &np);
	  mutex_lock (&dp->lock);
	  if (err)
	    goto out;
	  retry_dotdot = inum;
	  goto try_again;
	}

      /* Here below are the spec dotdot cases. */
      else if (type == RENAME || type == REMOVE)
	np = ifind (inum);

      else if (type == LOOKUP)
	{
	  diskfs_nput (dp);
	  err = diskfs_cached_lookup (inum, &np);
	  if (err)
	    goto out;
	}
      else
	assert (0);
    }

  if ((type == CREATE || type == RENAME) && !inum && ds && ds->stat == LOOKING)
    {
      /* We didn't find any room, so mark ds to extend the dir */
      ds->type = CREATE;
      ds->stat = EXTEND;
      ds->idx = dp->dn_stat.st_size / DIRBLKSIZ;
    }

  /* Return to the user; if we can't, release the reference
     (and lock) we acquired above.  */
 out:
  /* Deallocate or save the mapping. */
  if ((err && err != ENOENT)
      || !ds
      || ds->type == LOOKUP)
    {
      vm_deallocate (mach_task_self (), buf, buflen);
      if (ds)
	ds->type = LOOKUP;	/* set to be ignored by drop_dirstat */
    }
  else
    {
      ds->mapbuf = buf;
      ds->mapextent = buflen;
    }

  if (np)
    {
      assert (npp);
      if (err)
	{
	  if (!spec_dotdot)
	    {
	      /* Normal case */
	      if (np == dp)
		diskfs_nrele (np);
	      else
		diskfs_nput (np);
	    }
	  else if (type == RENAME || type == REMOVE)
	    /* We just did ifind to get np; that allocates
	       no new references, so we don't have anything to do */
	    ;
	  else if (type == LOOKUP)
	    /* We did diskfs_cached_lookup */
	    diskfs_nput (np);
	}
      else
	*npp = np;
    }

  return err ? : inum ? 0 : ENOENT;
}

/* Scan block at address BLKADDR (of node DP; block index IDX), for
   name NAME of length NAMELEN.  Args TYPE, DS are as for
   diskfs_lookup.  If found, set *INUM to the inode number, else
   return ENOENT.  */
static error_t
dirscanblock (vm_address_t blockaddr, struct node *dp, int idx,
	      const char *name, int namelen, enum lookup_type type,
	      struct dirstat *ds, ino_t *inum)
{
  int nfree = 0;
  int needed = 0;
  vm_address_t currentoff, prevoff;
  struct ext2_dir_entry *entry = 0;
  int nentries = 0;
  size_t nbytes = 0;
  int looking = 0;
  int countcopies = 0;
  int consider_compress = 0;

  if (ds && (ds->stat == LOOKING
	     || ds->stat == COMPRESS))
    {
      looking = 1;
      countcopies = 1;
      needed = EXT2_DIR_REC_LEN (namelen);
    }

  for (currentoff = blockaddr, prevoff = 0;
       currentoff < blockaddr + DIRBLKSIZ;
       prevoff = currentoff, currentoff += entry->rec_len)
    {
      entry = (struct ext2_dir_entry *)currentoff;

      if (!entry->rec_len
	  || entry->rec_len % EXT2_DIR_PAD
	  || entry->name_len > EXT2_NAME_LEN
	  || currentoff + entry->rec_len > blockaddr + DIRBLKSIZ
	  || EXT2_DIR_REC_LEN (entry->name_len) > entry->rec_len
	  || memchr (entry->name, '\0', entry->name_len))
	{
	  ext2_warning ("bad directory entry: inode: %d offset: %ld",
			dp->cache_id,
			currentoff - blockaddr + idx * DIRBLKSIZ);
	  return ENOENT;
	}

      if (looking || countcopies)
	{
	  int thisfree;

	  /* Count how much free space this entry has in it. */
	  if (entry->inode == 0)
	    thisfree = entry->rec_len;
	  else
	    thisfree = entry->rec_len - EXT2_DIR_REC_LEN (entry->name_len);

	  /* If this isn't at the front of the block, then it will
	     have to be copied if we do a compression; count the
	     number of bytes there too. */
	  if (countcopies && currentoff != blockaddr)
	    nbytes += EXT2_DIR_REC_LEN (entry->name_len);

	  if (ds->stat == COMPRESS && nbytes > ds->nbytes)
	    /* The previously found compress is better than
	       this one, so don't bother counting any more. */
	    countcopies = 0;

	  if (thisfree >= needed)
	    {
	      ds->type = CREATE;
	      ds->stat = entry->inode == 0 ? TAKE : SHRINK;
	      ds->entry = entry;
	      ds->idx = idx;
	      looking = countcopies = 0;
	    }
	  else
	    {
	      nfree += thisfree;
	      if (nfree >= needed)
		consider_compress = 1;
	    }
	}

      if (entry->inode)
	nentries++;

      if (entry->name_len == namelen
	  && entry->name[0] == name[0]
	  && entry->inode
	  && !bcmp (entry->name, name, namelen))
	break;
    }

  if (consider_compress
      && (ds->type == LOOKING
	  || (ds->type == COMPRESS && ds->nbytes > nbytes)))
    {
      ds->type = CREATE;
      ds->stat = COMPRESS;
      ds->entry = (struct ext2_dir_entry *) blockaddr;
      ds->idx = idx;
      ds->nbytes = nbytes;
    }

  if (currentoff >= blockaddr + DIRBLKSIZ)
    {
      int i;
      /* The name is not in this block. */

      /* Because we scanned the entire block, we should write
	 down how many entries there were. */
      if (!dp->dn->dirents)
	{
	  dp->dn->dirents = malloc ((dp->dn_stat.st_size / DIRBLKSIZ)
				    * sizeof (int));
	  for (i = 0; i < dp->dn_stat.st_size/DIRBLKSIZ; i++)
	    dp->dn->dirents[i] = -1;
	}
      /* Make sure the count is correct if there is one now. */
      assert (dp->dn->dirents[idx] == -1
	      || dp->dn->dirents[idx] == nentries);
      dp->dn->dirents[idx] = nentries;

      return ENOENT;
    }

  /* We have found the required name. */

  if (ds && type == CREATE)
    ds->type = LOOKUP;		/* it's invalid now */
  else if (ds && (type == REMOVE || type == RENAME))
    {
      ds->type = type;
      ds->stat = HERE_TIS;
      ds->entry = entry;
      ds->idx = idx;
      ds->preventry = (struct ext2_dir_entry *) prevoff;
    }

  *inum = entry->inode;
  return 0;
}

/* Following a lookup call for CREATE, this adds a node to a directory.
   DP is the directory to be modified; NAME is the name to be entered;
   NP is the node being linked in; DS is the cached information returned
   by lookup; CRED describes the user making the call.  This call may
   only be made if the directory has been held locked continuously since
   the preceding lookup call, and only if that call returned ENOENT. */
error_t
diskfs_direnter_hard (struct node *dp, const char *name, struct node *np,
		      struct dirstat *ds, struct protid *cred)
{
  struct ext2_dir_entry *new;
  int namelen = strlen (name);
  int needed = EXT2_DIR_REC_LEN (namelen);
  int oldneeded;
  vm_address_t fromoff, tooff;
  int totfreed;
  error_t err;
  off_t oldsize = 0;

  assert (ds->type == CREATE);

  assert (!diskfs_readonly);

  dp->dn_set_mtime = 1;

  switch (ds->stat)
    {
    case TAKE:
      /* We are supposed to consume this slot. */
      assert (ds->entry->inode == 0 && ds->entry->rec_len >= needed);

      ds->entry->inode = np->cache_id;
      ds->entry->name_len = namelen;
      bcopy (name, ds->entry->name, namelen);

      break;

    case SHRINK:
      /* We are supposed to take the extra space at the end
	 of this slot. */
      oldneeded = EXT2_DIR_REC_LEN (ds->entry->name_len);
      assert (ds->entry->rec_len - oldneeded >= needed);

      new = (struct ext2_dir_entry *) ((vm_address_t) ds->entry + oldneeded);

      new->inode = np->cache_id;
      new->rec_len = ds->entry->rec_len - oldneeded;
      new->name_len = namelen;
      bcopy (name, new->name, namelen);

      ds->entry->rec_len = oldneeded;

      break;

    case COMPRESS:
      /* We are supposed to move all the entries to the
	 front of the block, giving each the minimum
	 necessary room.  This should free up enough space
	 for the new entry. */
      fromoff = tooff = (vm_address_t) ds->entry;

      while (fromoff < (vm_address_t) ds->entry + DIRBLKSIZ)
	{
	  struct ext2_dir_entry *from = (struct ext2_dir_entry *)fromoff;
	  struct ext2_dir_entry *to = (struct ext2_dir_entry *) tooff;
	  int fromreclen = from->rec_len;

	  if (from->inode != 0)
	    {
	      assert (fromoff >= tooff);

	      bcopy (from, to, fromreclen);
	      to->rec_len = EXT2_DIR_REC_LEN (to->name_len);

	      tooff += to->rec_len;
	    }
	  fromoff += fromreclen;
	}

      totfreed = (vm_address_t) ds->entry + DIRBLKSIZ - tooff;
      assert (totfreed >= needed);

      new = (struct ext2_dir_entry *) tooff;
      new->inode = np->cache_id;
      new->rec_len = totfreed;
      new->name_len = namelen;
      bcopy (name, new->name, namelen);
      break;

    case EXTEND:
      /* Extend the file. */
      assert (needed <= DIRBLKSIZ);

      oldsize = dp->dn_stat.st_size;
      while (oldsize + DIRBLKSIZ > dp->allocsize)
	{
	  err = diskfs_grow (dp, oldsize + DIRBLKSIZ, cred);
	  if (err)
	    {
	      vm_deallocate (mach_task_self (), ds->mapbuf, ds->mapextent);
	      return err;
	    }
	}

      new = (struct ext2_dir_entry *) (ds->mapbuf + oldsize);

      dp->dn_stat.st_size = oldsize + DIRBLKSIZ;
      dp->dn_set_ctime = 1;

      new->inode = np->cache_id;
      new->rec_len = DIRBLKSIZ;
      new->name_len = namelen;
      bcopy (name, new->name, namelen);
      break;

    default:
      assert (0);
    }

  dp->dn_set_mtime = 1;

  vm_deallocate (mach_task_self (), ds->mapbuf, ds->mapextent);

  if (ds->stat != EXTEND)
    {
      /* If we are keeping count of this block, then keep the count up
	 to date. */
      if (dp->dn->dirents && dp->dn->dirents[ds->idx] != -1)
	dp->dn->dirents[ds->idx]++;
    }
  else
    {
      int i;
      /* It's cheap, so start a count here even if we aren't counting
	 anything at all. */
      if (dp->dn->dirents)
	{
	  dp->dn->dirents = realloc (dp->dn->dirents,
				     (dp->dn_stat.st_size / DIRBLKSIZ
				      * sizeof (int)));
	  for (i = oldsize / DIRBLKSIZ;
	       i < dp->dn_stat.st_size / DIRBLKSIZ;
	       i++)
	    dp->dn->dirents[i] = -1;

	  dp->dn->dirents[ds->idx] = 1;
	}
      else
	{
	  dp->dn->dirents = malloc (dp->dn_stat.st_size / DIRBLKSIZ
				    * sizeof (int));
	  for (i = 0; i < dp->dn_stat.st_size / DIRBLKSIZ; i++)
	    dp->dn->dirents[i] = -1;
	  dp->dn->dirents[ds->idx] = 1;
	}
    }

  diskfs_file_update (dp, 1);

  return 0;
}

/* Following a lookup call for REMOVE, this removes the link from the
   directory.  DP is the directory being changed and DS is the cached
   information returned from lookup.  This call is only valid if the
   directory has been locked continously since the call to lookup, and
   only if that call succeeded.  */
error_t
diskfs_dirremove_hard (struct node *dp, struct dirstat *ds)
{
  assert (ds->type == REMOVE);
  assert (ds->stat == HERE_TIS);

  assert (!diskfs_readonly);

  dp->dn_set_mtime = 1;

  if (ds->preventry == 0)
    ds->entry->inode = 0;
  else
    {
      assert ((vm_address_t) ds->entry - (vm_address_t) ds->preventry
	      == ds->preventry->rec_len);
      ds->preventry->rec_len += ds->entry->rec_len;
    }

  dp->dn_set_mtime = 1;

  vm_deallocate (mach_task_self (), ds->mapbuf, ds->mapextent);

  /* If we are keeping count of this block, then keep the count up
     to date. */
  if (dp->dn->dirents && dp->dn->dirents[ds->idx] != -1)
    dp->dn->dirents[ds->idx]--;

  diskfs_file_update (dp, 1);

  return 0;
}


/* Following a lookup call for RENAME, this changes the inode number
   on a directory entry.  DP is the directory being changed; NP is
   the new node being linked in; DP is the cached information returned
   by lookup.  This call is only valid if the directory has been locked
   continuously since the call to lookup, and only if that call
   succeeded.  */
error_t
diskfs_dirrewrite_hard (struct node *dp, struct node *np, struct dirstat *ds)
{
  assert (ds->type == RENAME);
  assert (ds->stat == HERE_TIS);

  assert (!diskfs_readonly);

  ds->entry->inode = np->cache_id;
  dp->dn_set_mtime = 1;

  vm_deallocate (mach_task_self (), ds->mapbuf, ds->mapextent);

  diskfs_file_update (dp, 1);

  return 0;
}

/* Tell if DP is an empty directory (has only "." and ".." entries).
   This routine must be called from inside a catch_exception ().  */
int
diskfs_dirempty (struct node *dp, struct protid *cred)
{
  error_t err;
  vm_address_t buf = 0, curoff;
  struct ext2_dir_entry *entry;
  int hit = 0;			/* Found something in the directory.  */
  memory_object_t memobj = diskfs_get_filemap (dp, VM_PROT_READ);

  if (memobj == MACH_PORT_NULL)
    /* XXX should reflect error properly. */
    return 0;

  err = vm_map (mach_task_self (), &buf, dp->dn_stat.st_size, 0,
		1, memobj, 0, 0, VM_PROT_READ, VM_PROT_READ, 0);
  mach_port_deallocate (mach_task_self (), memobj);
  assert (!err);

  if (! diskfs_check_readonly ())
    dp->dn_set_atime = 1;

  for (curoff = buf;
       !hit && curoff < buf + dp->dn_stat.st_size;
       curoff += entry->rec_len)
    {
      entry = (struct ext2_dir_entry *) curoff;

      if (entry->inode != 0
	  && (entry->name_len > 2
	      || entry->name[0] != '.'
	      || (entry->name[1] != '.'
		  && entry->name[1] != '\0')))
	hit = 1;
    }

  if (! diskfs_check_readonly ())
    dp->dn_set_atime = 1;
  if (diskfs_synchronous)
    diskfs_node_update (dp, 1);

  vm_deallocate (mach_task_self (), buf, dp->dn_stat.st_size);

  return !hit;
}

/* Make DS an invalid dirstat. */
error_t
diskfs_drop_dirstat (struct node *dp, struct dirstat *ds)
{
  if (ds->type != LOOKUP)
    {
      assert (ds->mapbuf);
      vm_deallocate (mach_task_self (), ds->mapbuf, ds->mapextent);
      ds->type = LOOKUP;
    }
  return 0;
}


/* Count the entries in directory block NB for directory DP and
   write the answer down in its dirents array.  As a side affect
   fill BUF with the block.  */
static error_t
count_dirents (struct node *dp, int nb, char *buf)
{
  int amt;
  char *offinblk;
  struct ext2_dir_entry *entry;
  int count = 0;
  error_t err;

  assert (dp->dn->dirents);
  assert ((nb + 1) * DIRBLKSIZ <= dp->dn_stat.st_size);

  err = diskfs_node_rdwr (dp, buf, nb * DIRBLKSIZ, DIRBLKSIZ, 0, 0, &amt);
  if (err)
    return err;
  assert (amt == DIRBLKSIZ);

  for (offinblk = buf;
       offinblk < buf + DIRBLKSIZ;
       offinblk += entry->rec_len)
    {
      entry = (struct ext2_dir_entry *) offinblk;
      if (entry->inode)
	count++;
    }

  assert (dp->dn->dirents[nb] == -1 || dp->dn->dirents[nb] == count);
  dp->dn->dirents[nb] = count;
  return 0;
}

/* Returned directory entries are aligned to blocks this many bytes long.
   Must be a power of two.  */
#define DIRENT_ALIGN 4

/* Implement the disikfs_get_directs callback as described in
   <hurd/diskfs.h>. */
error_t
diskfs_get_directs (struct node *dp,
		    int entry,
		    int nentries,
		    char **data,
		    u_int *datacnt,
		    vm_size_t bufsiz,
		    int *amt)
{
  int blkno;
  int nblks;
  int curentry;
  char buf[DIRBLKSIZ];
  char *bufp;
  int bufvalid;
  error_t err;
  int i;
  char *datap;
  struct ext2_dir_entry *entryp;
  int allocsize;
  int checklen;
  struct dirent *userp;

  nblks = dp->dn_stat.st_size/DIRBLKSIZ;

  if (!dp->dn->dirents)
    {
      dp->dn->dirents = malloc (nblks * sizeof (int));
      for (i = 0; i < nblks; i++)
	dp->dn->dirents[i] = -1;
    }

  /* Allocate enough space to hold the maximum we might return */
  if (!bufsiz || bufsiz > dp->dn_stat.st_size)
    /* Allocate enough to return the entire directory.  Since ext2's
       directory format is different than the format used to return the
       entries, we allocate enough to hold the on disk directory plus
       whatever extra would be necessary in the worst-case.  */
    {
      /* The minimum size of an ext2fs directory entry.  */
      size_t min_entry_size = EXT2_DIR_REC_LEN (0);
      /* The minimum size of a returned dirent entry.  The +1 is for '\0'.  */
      size_t min_dirent_size = offsetof (struct dirent, d_name) + 1;
      /* The maximum possible number of ext2fs dir entries in this dir.  */
      size_t max_entries = dp->dn_stat.st_size / min_entry_size;
      /* The maximum difference in size per directory entry.  */
      size_t entry_extra =
	DIRENT_ALIGN
	  + (min_dirent_size > min_entry_size
	     ? min_dirent_size - min_entry_size : 0);

      allocsize = round_page (dp->dn_stat.st_size + max_entries * entry_extra);
    }
  else
    allocsize = round_page (bufsiz);

  if (allocsize > *datacnt)
    vm_allocate (mach_task_self (), (vm_address_t *) data, allocsize, 1);

  /* Scan through the entries to find ENTRY.  If we encounter
     a -1 in the process then stop to fill it.  When we run
     off the end, ENTRY is too big. */
  curentry = 0;
  bufvalid = 0;
  for (blkno = 0; blkno < nblks; blkno++)
    {
      if (dp->dn->dirents[blkno] == -1)
	{
	  err = count_dirents (dp, blkno, buf);
	  if (err)
	    return err;
	  bufvalid = 1;
	}

      if (curentry + dp->dn->dirents[blkno] > entry)
	/* ENTRY starts in this block. */
	break;

      curentry += dp->dn->dirents[blkno];

      bufvalid = 0;
    }

  if (blkno == nblks)
    {
      *datacnt = 0;
      *amt = 0;
      return 0;
    }

  /* Set bufp appropriately */
  bufp = buf;
  if (curentry != entry)
    {
      /* Look through the block to find out where to start,
	 setting bufp appropriately.  */
      if (!bufvalid)
	{
	  err = diskfs_node_rdwr (dp, buf, blkno * DIRBLKSIZ, DIRBLKSIZ,
				  0, 0, &checklen);
	  if (err)
	    return err;
	  assert (checklen == DIRBLKSIZ);
	  bufvalid = 1;
	}
      for (i = 0, bufp = buf;
	   i < entry - curentry && bufp - buf < DIRBLKSIZ;
	   bufp += ((struct ext2_dir_entry *)bufp)->rec_len, i++)
	;
      /* Make sure we didn't run off the end. */
      assert (bufp - buf < DIRBLKSIZ);
    }

  i = 0;
  datap = *data;

  /* Copy the entries, one at a time. */
  while (((nentries == -1) || (i < nentries))
	 && (!bufsiz || (datap - *data < bufsiz) )
	 && blkno < nblks)
    {
      if (!bufvalid)
	{
	  err = diskfs_node_rdwr (dp, buf, blkno * DIRBLKSIZ, DIRBLKSIZ,
				  0, 0, &checklen);
	  if (err)
	    return err;
	  assert (checklen == DIRBLKSIZ);
	  bufvalid = 1;
	  bufp = buf;
	}

      entryp = (struct ext2_dir_entry *)bufp;

      if (entryp->inode)
	{
	  int rec_len;
	  int name_len = entryp->name_len;

	  userp = (struct dirent *) datap;

	  /* Length is structure before the name + the name + '\0', all
	     padded to a four-byte alignment.  */
	  rec_len =
	    ((offsetof (struct dirent, d_name)
	      + name_len + 1
	      + (DIRENT_ALIGN - 1))
	     & ~(DIRENT_ALIGN - 1));

	  /* See if this record would run over the end of the return buffer. */
	  if (bufsiz == 0)
	    /* It shouldn't ever, as we calculated the worst case size.  */
	    assert (datap + rec_len <= *data + allocsize);
	  else
	    /* It's ok if it does, just leave off returning this entry.  */
	    if (datap + rec_len > *data + allocsize)
	      break;

	  userp->d_fileno = entryp->inode;
	  userp->d_reclen = rec_len;
	  userp->d_namlen = name_len;
	  bcopy (entryp->name, userp->d_name, name_len);
	  userp->d_name[name_len] = '\0';

	  datap += rec_len;
	  i++;
	}

      if (entryp->rec_len == 0)
	{
	  ext2_warning ("zero length directory entry: inode: %d offset: %d",
			dp->cache_id,
			blkno * DIRBLKSIZ + bufp - buf);
	  return EIO;
	}

      bufp += entryp->rec_len;
      if (bufp - buf == DIRBLKSIZ)
	{
	  blkno++;
	  bufvalid = 0;
	}
      else if (bufp - buf > DIRBLKSIZ)
	{
	  ext2_warning ("directory entry too long: inode: %d offset: %d",
			dp->cache_id,
			blkno * DIRBLKSIZ + bufp - buf - entryp->rec_len);
	  return EIO;
	}
    }

  /* We've copied all we can.  If we allocated our own array
     but didn't fill all of it, then free whatever memory we didn't use. */
  if (allocsize > *datacnt)
    {
      if (round_page (datap - *data) < allocsize)
	vm_deallocate (mach_task_self (),
		       (vm_address_t) (*data + round_page (datap - *data)),
		       allocsize - round_page (datap - *data));
    }

  /* Set variables for return */
  *datacnt = datap - *data;
  *amt = i;
  return 0;
}
