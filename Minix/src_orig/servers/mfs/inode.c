/* This file manages the inode table.  There are procedures to allocate and
 * deallocate inodes, acquire, erase, and release them, and read and write
 * them from the disk.
 *
 * The entry points into this file are
 *   get_inode:	   search inode table for a given inode; if not there,
 *                 read it
 *   put_inode:	   indicate that an inode is no longer needed in memory
 *   alloc_inode:  allocate a new, unused inode
 *   wipe_inode:   erase some fields of a newly allocated inode
 *   free_inode:   mark an inode as available for a new file
 *   update_times: update atime, ctime, and mtime
 *   rw_inode:	   read a disk block and extract an inode, or corresp. write
 *   dup_inode:	   indicate that someone else is using an inode table entry
 *   find_inode:   retrieve pointer to inode in inode cache
 *
 */

#include "fs.h"
#include "buf.h"
#include "inode.h"
#include "super.h"
#include <minix/vfsif.h>

FORWARD _PROTOTYPE( void addhash_inode, (struct inode *node)		); 

FORWARD _PROTOTYPE( void free_inode, (dev_t dev, ino_t numb)			);
FORWARD _PROTOTYPE( void new_icopy, (struct inode *rip, d2_inode *dip,
						int direction, int norm));
FORWARD _PROTOTYPE( void old_icopy, (struct inode *rip, d1_inode *dip,
						int direction, int norm));
FORWARD _PROTOTYPE( void unhash_inode, (struct inode *node) 		);
FORWARD _PROTOTYPE( void wipe_inode, (struct inode *rip)			);


/*===========================================================================*
 *				fs_putnode				     *
 *===========================================================================*/
PUBLIC int fs_putnode(void)
{
/* Find the inode specified by the request message and decrease its counter.*/

  struct inode *rip;
  int count;
  
  rip = find_inode(fs_dev, (ino_t) fs_m_in.REQ_INODE_NR);

  if(!rip) {
	  printf("%s:%d put_inode: inode #%lu dev: %d not found\n", __FILE__,
		 __LINE__, (ino_t) fs_m_in.REQ_INODE_NR, fs_dev);
	  panic("fs_putnode failed");
  }

  count = fs_m_in.REQ_COUNT;
  if (count <= 0) {
	printf("%s:%d put_inode: bad value for count: %d\n", __FILE__,
	       __LINE__, count);
	panic("fs_putnode failed");
  } else if(count > rip->i_count) {
	printf("%s:%d put_inode: count too high: %d > %d\n", __FILE__,
	       __LINE__, count, rip->i_count);
	panic("fs_putnode failed");
  }

  /* Decrease reference counter, but keep one reference; it will be consumed by
   * put_inode(). */ 
  rip->i_count -= count - 1;
  put_inode(rip);

  return(OK);
}


/*===========================================================================*
 *				init_inode_cache			     *
 *===========================================================================*/
PUBLIC void init_inode_cache()
{
  struct inode *rip;
  struct inodelist *rlp;

  inode_cache_hit = 0;
  inode_cache_miss = 0;

  /* init free/unused list */
  TAILQ_INIT(&unused_inodes);
  
  /* init hash lists */
  for (rlp = &hash_inodes[0]; rlp < &hash_inodes[INODE_HASH_SIZE]; ++rlp) 
      LIST_INIT(rlp);

  /* add free inodes to unused/free list */
  for (rip = &inode[0]; rip < &inode[NR_INODES]; ++rip) {
      rip->i_num = NO_ENTRY;
      TAILQ_INSERT_HEAD(&unused_inodes, rip, i_unused);
  }
}


/*===========================================================================*
 *				addhash_inode   			     *
 *===========================================================================*/
PRIVATE void addhash_inode(struct inode *node) 
{
  int hashi = (int) (node->i_num & INODE_HASH_MASK);
  
  /* insert into hash table */
  LIST_INSERT_HEAD(&hash_inodes[hashi], node, i_hash);
}


/*===========================================================================*
 *				unhash_inode      			     *
 *===========================================================================*/
PRIVATE void unhash_inode(struct inode *node) 
{
  /* remove from hash table */
  LIST_REMOVE(node, i_hash);
}


/*===========================================================================*
 *				get_inode				     *
 *===========================================================================*/
PUBLIC struct inode *get_inode(
  dev_t dev,			/* device on which inode resides */
  ino_t numb			/* inode number */
)
{
/* Find the inode in the hash table. If it is not there, get a free inode
 * load it from the disk if it's necessary and put on the hash list 
 */
  register struct inode *rip;
  int hashi;

  hashi = (int) (numb & INODE_HASH_MASK);

  /* Search inode in the hash table */
  LIST_FOREACH(rip, &hash_inodes[hashi], i_hash) {
      if (rip->i_num == numb && rip->i_dev == dev) {
          /* If unused, remove it from the unused/free list */
          if (rip->i_count == 0) {
	      inode_cache_hit++;
              TAILQ_REMOVE(&unused_inodes, rip, i_unused);
	  }
          ++rip->i_count;
          return(rip);
      }
  }

  inode_cache_miss++;

  /* Inode is not on the hash, get a free one */
  if (TAILQ_EMPTY(&unused_inodes)) {
      err_code = ENFILE;
      return(NULL);
  }
  rip = TAILQ_FIRST(&unused_inodes);

  /* If not free unhash it */
  if (rip->i_num != NO_ENTRY)
      unhash_inode(rip);
  
  /* Inode is not unused any more */
  TAILQ_REMOVE(&unused_inodes, rip, i_unused);

  /* Load the inode. */
  rip->i_dev = dev;
  rip->i_num = numb;
  rip->i_count = 1;
  if (dev != NO_DEV) rw_inode(rip, READING);	/* get inode from disk */
  rip->i_update = 0;		/* all the times are initially up-to-date */
  rip->i_zsearch = NO_ZONE;	/* no zones searched for yet */
  rip->i_mountpoint= FALSE;

  /* Add to hash */
  addhash_inode(rip);
  
  return(rip);
}


/*===========================================================================*
 *				find_inode        			     *
 *===========================================================================*/
PUBLIC struct inode *find_inode(
  dev_t dev,			/* device on which inode resides */
  ino_t numb			/* inode number */
)
{
/* Find the inode specified by the inode and device number.
 */
  struct inode *rip;
  int hashi;

  hashi = (int) (numb & INODE_HASH_MASK);

  /* Search inode in the hash table */
  LIST_FOREACH(rip, &hash_inodes[hashi], i_hash) {
      if (rip->i_count > 0 && rip->i_num == numb && rip->i_dev == dev) {
          return(rip);
      }
  }
  
  return(NULL);
}


/*===========================================================================*
 *				put_inode				     *
 *===========================================================================*/
PUBLIC void put_inode(rip)
register struct inode *rip;	/* pointer to inode to be released */
{
/* The caller is no longer using this inode.  If no one else is using it either
 * write it back to the disk immediately.  If it has no links, truncate it and
 * return it to the pool of available inodes.
 */

  if (rip == NULL) return;	/* checking here is easier than in caller */

  if (rip->i_count < 1)
	panic("put_inode: i_count already below 1: %d", rip->i_count);

  if (--rip->i_count == 0) {	/* i_count == 0 means no one is using it now */
	if (rip->i_nlinks == NO_LINK) {
		/* i_nlinks == NO_LINK means free the inode. */
		/* return all the disk blocks */

		/* Ignore errors by truncate_inode in case inode is a block
		 * special or character special file.
		 */
		(void) truncate_inode(rip, (off_t) 0); 
		rip->i_mode = I_NOT_ALLOC;     /* clear I_TYPE field */
		rip->i_dirt = DIRTY;
		free_inode(rip->i_dev, rip->i_num);
	} 

        rip->i_mountpoint = FALSE;
	if (rip->i_dirt == DIRTY) rw_inode(rip, WRITING);

	if (rip->i_nlinks == NO_LINK) {
		/* free, put at the front of the LRU list */
		unhash_inode(rip);
		rip->i_num = NO_ENTRY;
		TAILQ_INSERT_HEAD(&unused_inodes, rip, i_unused);
	} else {
		/* unused, put at the back of the LRU (cache it) */
		TAILQ_INSERT_TAIL(&unused_inodes, rip, i_unused);
	}
  }
}


/*===========================================================================*
 *				alloc_inode				     *
 *===========================================================================*/
PUBLIC struct inode *alloc_inode(dev_t dev, mode_t bits)
{
/* Allocate a free inode on 'dev', and return a pointer to it. */

  register struct inode *rip;
  register struct super_block *sp;
  int major, minor, inumb;
  bit_t b;

  sp = get_super(dev);	/* get pointer to super_block */
  if (sp->s_rd_only) {	/* can't allocate an inode on a read only device. */
	err_code = EROFS;
	return(NULL);
  }

  /* Acquire an inode from the bit map. */
  b = alloc_bit(sp, IMAP, sp->s_isearch);
  if (b == NO_BIT) {
	err_code = ENOSPC;
	major = (int) (sp->s_dev >> MAJOR) & BYTE;
	minor = (int) (sp->s_dev >> MINOR) & BYTE;
	printf("Out of i-nodes on device %d/%d\n", major, minor);
	return(NULL);
  }
  sp->s_isearch = b;		/* next time start here */
  inumb = (int) b;		/* be careful not to pass unshort as param */

  /* Try to acquire a slot in the inode table. */
  if ((rip = get_inode(NO_DEV, inumb)) == NULL) {
	/* No inode table slots available.  Free the inode just allocated. */
	free_bit(sp, IMAP, b);
  } else {
	/* An inode slot is available. Put the inode just allocated into it. */
	rip->i_mode = bits;		/* set up RWX bits */
	rip->i_nlinks = NO_LINK;	/* initial no links */
	rip->i_uid = caller_uid;	/* file's uid is owner's */
	rip->i_gid = caller_gid;	/* ditto group id */
	rip->i_dev = dev;		/* mark which device it is on */
	rip->i_ndzones = sp->s_ndzones;	/* number of direct zones */
	rip->i_nindirs = sp->s_nindirs;	/* number of indirect zones per blk*/
	rip->i_sp = sp;			/* pointer to super block */

	/* Fields not cleared already are cleared in wipe_inode().  They have
	 * been put there because truncate() needs to clear the same fields if
	 * the file happens to be open while being truncated.  It saves space
	 * not to repeat the code twice.
	 */
	wipe_inode(rip);
  }

  return(rip);
}


/*===========================================================================*
 *				wipe_inode				     *
 *===========================================================================*/
PRIVATE void wipe_inode(rip)
register struct inode *rip;	/* the inode to be erased */
{
/* Erase some fields in the inode.  This function is called from alloc_inode()
 * when a new inode is to be allocated, and from truncate(), when an existing
 * inode is to be truncated.
 */

  register int i;

  rip->i_size = 0;
  rip->i_update = ATIME | CTIME | MTIME;	/* update all times later */
  rip->i_dirt = DIRTY;
  for (i = 0; i < V2_NR_TZONES; i++) rip->i_zone[i] = NO_ZONE;
}

/*===========================================================================*
 *				free_inode				     *
 *===========================================================================*/
PRIVATE void free_inode(
  dev_t dev,			/* on which device is the inode? */
  ino_t inumb			/* number of the inode to be freed */
)
{
/* Return an inode to the pool of unallocated inodes. */

  register struct super_block *sp;
  bit_t b;

  /* Locate the appropriate super_block. */
  sp = get_super(dev);
  if (inumb == NO_ENTRY || inumb > sp->s_ninodes) return;
  b = (bit_t) inumb;
  free_bit(sp, IMAP, b);
  if (b < sp->s_isearch) sp->s_isearch = b;
}


/*===========================================================================*
 *				update_times				     *
 *===========================================================================*/
PUBLIC void update_times(rip)
register struct inode *rip;	/* pointer to inode to be read/written */
{
/* Various system calls are required by the standard to update atime, ctime,
 * or mtime.  Since updating a time requires sending a message to the clock
 * task--an expensive business--the times are marked for update by setting
 * bits in i_update.  When a stat, fstat, or sync is done, or an inode is 
 * released, update_times() may be called to actually fill in the times.
 */

  time_t cur_time;
  struct super_block *sp;

  sp = rip->i_sp;		/* get pointer to super block. */
  if (sp->s_rd_only) return;	/* no updates for read-only file systems */

  cur_time = clock_time();
  if (rip->i_update & ATIME) rip->i_atime = cur_time;
  if (rip->i_update & CTIME) rip->i_ctime = cur_time;
  if (rip->i_update & MTIME) rip->i_mtime = cur_time;
  rip->i_update = 0;		/* they are all up-to-date now */
}

/*===========================================================================*
 *				rw_inode				     *
 *===========================================================================*/
PUBLIC void rw_inode(rip, rw_flag)
register struct inode *rip;	/* pointer to inode to be read/written */
int rw_flag;			/* READING or WRITING */
{
/* An entry in the inode table is to be copied to or from the disk. */

  register struct buf *bp;
  register struct super_block *sp;
  d1_inode *dip;
  d2_inode *dip2;
  block_t b, offset;

  /* Get the block where the inode resides. */
  sp = get_super(rip->i_dev);	/* get pointer to super block */
  rip->i_sp = sp;		/* inode must contain super block pointer */
  offset = START_BLOCK + sp->s_imap_blocks + sp->s_zmap_blocks;
  b = (block_t) (rip->i_num - 1)/sp->s_inodes_per_block + offset;
  bp = get_block(rip->i_dev, b, NORMAL);
  dip  = bp->b_v1_ino + (rip->i_num - 1) % V1_INODES_PER_BLOCK;
  dip2 = bp->b_v2_ino + (rip->i_num - 1) %
  	 V2_INODES_PER_BLOCK(sp->s_block_size);

  /* Do the read or write. */
  if (rw_flag == WRITING) {
	if (rip->i_update) update_times(rip);	/* times need updating */
	if (sp->s_rd_only == FALSE) bp->b_dirt = DIRTY;
  }

  /* Copy the inode from the disk block to the in-core table or vice versa.
   * If the fourth parameter below is FALSE, the bytes are swapped.
   */
  if (sp->s_version == V1)
	old_icopy(rip, dip,  rw_flag, sp->s_native);
  else
	new_icopy(rip, dip2, rw_flag, sp->s_native);
  
  put_block(bp, INODE_BLOCK);
  rip->i_dirt = CLEAN;
}


/*===========================================================================*
 *				old_icopy				     *
 *===========================================================================*/
PRIVATE void old_icopy(rip, dip, direction, norm)
register struct inode *rip;	/* pointer to the in-core inode struct */
register d1_inode *dip;		/* pointer to the d1_inode inode struct */
int direction;			/* READING (from disk) or WRITING (to disk) */
int norm;			/* TRUE = do not swap bytes; FALSE = swap */
{
/* The V1.x IBM disk, the V1.x 68000 disk, and the V2 disk (same for IBM and
 * 68000) all have different inode layouts.  When an inode is read or written
 * this routine handles the conversions so that the information in the inode
 * table is independent of the disk structure from which the inode came.
 * The old_icopy routine copies to and from V1 disks.
 */

  int i;

  if (direction == READING) {
	/* Copy V1.x inode to the in-core table, swapping bytes if need be. */
	rip->i_mode    = (mode_t) conv2(norm, (int) dip->d1_mode);
	rip->i_uid     = (uid_t)  conv2(norm, (int) dip->d1_uid );
	rip->i_size    = (off_t)  conv4(norm,       dip->d1_size);
	rip->i_mtime   = (time_t) conv4(norm,       dip->d1_mtime);
	rip->i_atime   = (time_t) rip->i_mtime;
	rip->i_ctime   = (time_t) rip->i_mtime;
	rip->i_nlinks  = (nlink_t) dip->d1_nlinks;	/* 1 char */
	rip->i_gid     = (gid_t) dip->d1_gid;		/* 1 char */
	rip->i_ndzones = V1_NR_DZONES;
	rip->i_nindirs = V1_INDIRECTS;
	for (i = 0; i < V1_NR_TZONES; i++)
		rip->i_zone[i] = (zone_t) conv2(norm, (int) dip->d1_zone[i]);
  } else {
	/* Copying V1.x inode to disk from the in-core table. */
	dip->d1_mode   = (u16_t) conv2(norm, (int) rip->i_mode);
	dip->d1_uid    = (i16_t) conv2(norm, (int) rip->i_uid );
	dip->d1_size   = (i32_t) conv4(norm,       rip->i_size);
	dip->d1_mtime  = (i32_t) conv4(norm,       rip->i_mtime);
	dip->d1_nlinks = (u8_t) rip->i_nlinks;		/* 1 char */
	dip->d1_gid    = (u8_t) rip->i_gid;		/* 1 char */
	for (i = 0; i < V1_NR_TZONES; i++)
		dip->d1_zone[i] = (u16_t) conv2(norm, (int) rip->i_zone[i]);
  }
}


/*===========================================================================*
 *				new_icopy				     *
 *===========================================================================*/
PRIVATE void new_icopy(rip, dip, direction, norm)
register struct inode *rip;	/* pointer to the in-core inode struct */
register d2_inode *dip;	/* pointer to the d2_inode struct */
int direction;			/* READING (from disk) or WRITING (to disk) */
int norm;			/* TRUE = do not swap bytes; FALSE = swap */
{
/* Same as old_icopy, but to/from V2 disk layout. */

  int i;

  if (direction == READING) {
	/* Copy V2.x inode to the in-core table, swapping bytes if need be. */
	rip->i_mode    = (mode_t) conv2(norm,dip->d2_mode);
	rip->i_uid     = (uid_t) conv2(norm,dip->d2_uid);
	rip->i_nlinks  = (nlink_t) conv2(norm,dip->d2_nlinks);
	rip->i_gid     = (gid_t) conv2(norm,dip->d2_gid);
	rip->i_size    = (off_t) conv4(norm,dip->d2_size);
	rip->i_atime   = (time_t) conv4(norm,dip->d2_atime);
	rip->i_ctime   = (time_t) conv4(norm,dip->d2_ctime);
	rip->i_mtime   = (time_t) conv4(norm,dip->d2_mtime);
	rip->i_ndzones = V2_NR_DZONES;
	rip->i_nindirs = V2_INDIRECTS(rip->i_sp->s_block_size);
	for (i = 0; i < V2_NR_TZONES; i++)
		rip->i_zone[i] = (zone_t) conv4(norm, (long) dip->d2_zone[i]);
  } else {
	/* Copying V2.x inode to disk from the in-core table. */
	dip->d2_mode   = (u16_t) conv2(norm,rip->i_mode);
	dip->d2_uid    = (i16_t) conv2(norm,rip->i_uid);
	dip->d2_nlinks = (u16_t) conv2(norm,rip->i_nlinks);
	dip->d2_gid    = (u16_t) conv2(norm,rip->i_gid);
	dip->d2_size   = (i32_t) conv4(norm,rip->i_size);
	dip->d2_atime  = (i32_t) conv4(norm,rip->i_atime);
	dip->d2_ctime  = (i32_t) conv4(norm,rip->i_ctime);
	dip->d2_mtime  = (i32_t) conv4(norm,rip->i_mtime);
	for (i = 0; i < V2_NR_TZONES; i++)
		dip->d2_zone[i] = (zone_t) conv4(norm, (long) rip->i_zone[i]);
  }
}


/*===========================================================================*
 *				dup_inode				     *
 *===========================================================================*/
PUBLIC void dup_inode(ip)
struct inode *ip;		/* The inode to be duplicated. */
{
/* This routine is a simplified form of get_inode() for the case where
 * the inode pointer is already known.
 */

  ip->i_count++;
}
