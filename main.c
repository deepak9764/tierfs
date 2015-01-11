/*
 * Copyright (c) 1998-2014 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2014 Stony Brook University
 * Copyright (c) 2003-2014 The Research Foundation of SUNY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "wrapfs.h"
#include <linux/module.h>

/*
 * our custom d_alloc_root work-alike
 *
 * we can't use d_alloc_root if we want to use our own interpose function
 * unchanged, so we simply call our own "fake" d_alloc_root
 */
static struct dentry *wrapfs_d_alloc_root(struct super_block *sb)
{
	struct dentry *ret = NULL;
	printk(KERN_INFO "In wrapfs_d_alloc_root");
	if (sb) {
		static const struct qstr name = {
			.name = "/",
			.len = 1
		};

		ret = d_alloc(NULL, &name);
		if (ret) {
			ret->d_op = &wrapfs_dops;
			ret->d_sb = sb;
			ret->d_parent = ret;
		}
	}
	return ret;
}

/*
 * There is no need to lock the wrapfs_super_info's rwsem as there is no
 * way anyone can have a reference to the superblock at this point in time.
 */
static int wrapfs_read_super(struct super_block *sb, void *raw_data, int silent)
{
	int err = 0;
	struct super_block *lower_sb;
	struct path lower_path;
	char *dev_name = (char *) raw_data;


	if (!dev_name) {
		printk(KERN_ERR "wrapfs: read_super: missing dev_name argument\n");
		err = -EINVAL;
		goto out;
	}

	/* parse lower path */
	err = kern_path(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
			&lower_path);
	if (err) {
		printk(KERN_ERR	"wrapfs: error accessing "
		       "lower directory '%s'\n", dev_name);
		goto out;
	}

	/* allocate superblock private data */
	sb->s_fs_info = kzalloc(sizeof(struct wrapfs_sb_info), GFP_KERNEL);
	if (!WRAPFS_SB(sb)) {
		printk(KERN_CRIT "wrapfs: read_super: out of memory\n");
		err = -ENOMEM;
		goto out_free;
	}

	/* set the lower superblock field of upper superblock */
	lower_sb = lower_path.dentry->d_sb;
	atomic_inc(&lower_sb->s_active);
	wrapfs_set_lower_super(sb, lower_sb);

	/* inherit maxbytes from lower file system */
	sb->s_maxbytes = lower_sb->s_maxbytes;

	/*
	 * Our c/m/atime granularity is 1 ns because we may stack on file
	 * systems whose granularity is as good.
	 */
	sb->s_time_gran = 1;

	sb->s_op = &wrapfs_sops;

	/* see comment next to the definition of wrapfs_d_alloc_root */
	sb->s_root = wrapfs_d_alloc_root(sb);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out_sput;
	}

	/* link the upper and lower dentries */
	sb->s_root->d_fsdata = NULL;
	err = new_dentry_private_data(sb->s_root);
	if (err)
		goto out_freeroot;

	/* set the lower dentries for s_root */
	wrapfs_set_lower_path(sb->s_root, &lower_path);
	
	/* call interpose to create the upper level inode */
	err = wrapfs_interpose(sb->s_root, sb, &lower_path);
	if (!err) {
		if (!silent)
		{
			printk(KERN_INFO"wrapfs: mounted on top of %s type %s\n",dev_name, lower_sb->s_type->name);
		}
		goto out;
	}
	/* else error: fall through */

	free_dentry_private_data(sb->s_root);
out_freeroot:
	dput(sb->s_root);
out_sput:
	/* drop refs we took earlier */
	atomic_dec(&lower_sb->s_active);
	kfree(WRAPFS_SB(sb));
	sb->s_fs_info = NULL;
out_free:
	path_put(&lower_path);

out:
	return err;
}

void wrapfs_tier_mount(char * parse_string)
{
	int i,j,k,n,err,fl;
	struct path tier[5];
	struct super_block *root_sb;
	struct dentry tier_dentry[5];
	char **t_path;
	j=0,k=0,n=0,fl=0;
		
	/*allocate memory for storing the tier pathname*/
	t_path=(char **)kmalloc(sizeof(char *)*3,GFP_KERNEL);
	for(i=0;i<3;i++)
	{
		t_path[i]=(char *)kmalloc(sizeof (char)*30,GFP_KERNEL);
	}

	/*parse the string*/
	fl=0;
	for(i=0;parse_string[i]!='\0';i++)
	{
		if(parse_string[i]==',')
		{
			t_path[j][k]='\0';

			/*perform lookup on pathname in t_path[j] to get path structure in tier[n]*/
			err=kern_path(t_path[j],LOOKUP_FOLLOW,&tier[n]);
			j++;
			k=0;
			n++;
			fl=0;			
		}
		else if(fl==1)
		{
			t_path[j][k]=parse_string[i];
			k++;
		}
		else if(parse_string[i]==':')
			fl=1;
		else
		{}
		
	}
	
	/*for last path*/
	t_path[j][k]='\0';
	err=kern_path(t_path[j],LOOKUP_FOLLOW,&tier[n]);
	
    /*get superblock object of parent in root_sb*/
	root_sb=tier[0].mnt->mnt_parent->mnt_sb;

	/*point the private field of that superblock to array of path structures-tier*/
	WRAPFS_SB(root_sb)->abhi_pvt=tier;
	
	/*for(i=0;i<n+1;i++)
	{
		tier[i].dentry->d_fsdata=tier[i].mnt->mnt_sb;
	}*/
	//for(i=0;i<3;i++)
	//	printk("\n\ntier1:%s",((struct path *)(WRAPFS_SB(root_sb)->abhi_pvt)+i)->mnt->mnt_devname);
}

static int wrapfs_get_sb(struct file_system_type *fs_type,
			 int flags, const char *dev_name,
			 void *raw_data, struct vfsmount *mnt)
{
	int err;
	void *lower_path_name = (void *) dev_name;
	char *data= (char *) raw_data;
	err = get_sb_nodev(fs_type, flags, lower_path_name,wrapfs_read_super, mnt);
	wrapfs_tier_mount(data);
	return err;
}

static struct file_system_type wrapfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= WRAPFS_NAME,
	.get_sb		= wrapfs_get_sb,
	.kill_sb	= generic_shutdown_super,
	.fs_flags	= FS_REVAL_DOT,
};

static int __init init_wrapfs_fs(void)
{
	int err;

	pr_info("Registering wrapfs " WRAPFS_VERSION "\n");

	err = wrapfs_init_inode_cache();
	if (err)
		goto out;
	err = wrapfs_init_dentry_cache();
	if (err)
		goto out;
	err = register_filesystem(&wrapfs_fs_type);
out:
	if (err) {
		wrapfs_destroy_inode_cache();
		wrapfs_destroy_dentry_cache();
	}
	return err;
}

static void __exit exit_wrapfs_fs(void)
{
	wrapfs_destroy_inode_cache();
	wrapfs_destroy_dentry_cache();
	unregister_filesystem(&wrapfs_fs_type);
	pr_info("Completed wrapfs module unload\n");
}

MODULE_AUTHOR("Erez Zadok, Filesystems and Storage Lab, Stony Brook University"
	      " (http://www.fsl.cs.sunysb.edu/)");
MODULE_DESCRIPTION("Wrapfs " WRAPFS_VERSION
		   " (http://wrapfs.filesystems.org/)");
MODULE_LICENSE("GPL");

module_init(init_wrapfs_fs);
module_exit(exit_wrapfs_fs);
