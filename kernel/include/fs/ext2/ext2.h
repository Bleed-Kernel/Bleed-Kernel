#pragma once

#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>


#define EXT2_SUPER_MAGIC        0xEF53

#define EXT2_SUPERBLOCK_OFFSET  1024
#define EXT2_SUPERBLOCK_SIZE    1024

#define EXT2_S_IFREG   0x8000   // regular file          
#define EXT2_S_IFDIR   0x4000   // directory             
#define EXT2_S_IFLNK   0xA000   // symbolic link         
#define EXT2_S_IFMT    0xF000   // type mask             

#define EXT2_ROOT_INO  2        // root directory inode  

// Directory entry file types 
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

// Block group descriptor table starts at block 1
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;        // total inodes               
    uint32_t s_blocks_count;        // total blocks               
    uint32_t s_r_blocks_count;      // reserved blocks            
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;    // first block id (0 or 1)    
    uint32_t s_log_block_size;      // block size = 1024 << this  
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;               // must be EXT2_SUPER_MAGIC   
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;           // 0 = original, 1 = dynamic  
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT2_DYNAMIC_REV fields (rev_level >= 1) 
    uint32_t s_first_ino;           // first non-reserved inode   
    uint16_t s_inode_size;          // size of inode structure     
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // padding to 1024 bytes 
    uint8_t  _pad[820];
} ext2_superblock_t;


typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;       // block number of block bitmap 
    uint32_t bg_inode_bitmap;       // block number of inode bitmap 
    uint32_t bg_inode_table;        // block number of inode table  
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgd_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;                // lower 32 bits of file size  
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;              // 512-byte block count        
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];           // 12 direct + ind + dind + tind 
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;             // upper 32 bits of size for regular files (rev1) 
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_disk_inode_t;
 
typedef struct __attribute__((packed)) {
    uint32_t inode;                 // 0 = unused entry            
    uint16_t rec_len;               // distance to next entry      
    uint8_t  name_len;
    uint8_t  file_type;
    // char name[] follows 
} ext2_dirent_t;

typedef struct {
    INode_t  *dev;                  // backing block device inode  
    uint32_t  block_size;           // bytes per block             
    uint32_t  blocks_per_group;
    uint32_t  inodes_per_group;
    uint32_t  inode_size;           // bytes per on-disk inode     
    uint32_t  first_data_block;     // 0 for bs>1024, 1 for bs==1024 
    uint32_t  num_groups;
    uint32_t  total_inodes;
    // cached superblock copy 
    ext2_superblock_t sb;
} ext2_fs_t;

typedef struct {
    ext2_fs_t       *fs;
    uint32_t         ino;           // 1-based inode number        
    ext2_disk_inode_t disk;         // cached on-disk inode        
} ext2_inode_t;

int ext2_mount(INode_t *dev_inode, INode_t **root);