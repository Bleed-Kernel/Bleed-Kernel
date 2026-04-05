#pragma once

#include <fs/vfs.h>
#include <stddef.h>
#include <stdint.h>

#define TEMPFS_MAX_NAME_LEN         128
#define TEMPFS_DATA_CHUNK_SIZE      4096
#define MAX_ENTRIES_PER_DATA_CHUNK  ((TEMPFS_DATA_CHUNK_SIZE-sizeof(tempfs_data_t))/sizeof(INode_t*))
#define MAX_FILE_DATA_PER_CHUNK     ((TEMPFS_DATA_CHUNK_SIZE-sizeof(tempfs_data_t)))

#define file_data(data)((char*)((data)+1))
#define directory_entries(data)((INode_t**)((data)+1))

typedef struct tempfs_data{
    struct tempfs_data* next_chunk;
}tempfs_data_t;

typedef struct tempfs_INode{
    char name[TEMPFS_MAX_NAME_LEN];
    size_t capacity;
    tempfs_data_t* data;
}tempfs_INode_t;

extern const INodeOps_t dir_ops;
extern const INodeOps_t file_ops;
extern const filesystem tempfs;

/// @brief Create a new inode, store operations inside
/// @param type file or directory
/// @param ops operations pointer
/// @return new inode
INode_t* tempfs_create_inode(int type, const INodeOps_t* ops);

/// @brief release an inode's internal data (does not free the INode_t itself)
/// @param inode target inode
void tempfs_drop(INode_t* inode);

/// @brief create chunk of data and return its structure pointer
/// @return new structure pointer
tempfs_data_t* tempfs_new_data_chunk();

/// @brief Look for an inode given as a child of an inode directory
/// @param dir parent
/// @param name name
/// @param namelen name length
/// @param result inode of result
/// @return success
int tempfs_lookup(INode_t* dir, const char* name, size_t namelen, INode_t** result);

/// @brief read an inodes data out to a pointer
/// @param inode inode to read
/// @param out_buffer output buffer
/// @param count size to read
/// @param offset read offset
/// @return read size (negitive indicates failure) 
long tempfs_read(INode_t* inode, void* out_buffer, size_t count, size_t offset);

/// @brief write to an inodes data
/// @param inode target inode
/// @param in_buffer data to write
/// @param count ammount of data to write
/// @param offset write offset in inode
/// @return write size (negitive indicates failure) 
long tempfs_write(INode_t* inode, const void* in_buffer, size_t count, size_t offset);

/// @brief Create a new file inside a directory
/// @param parent directory inode
/// @param name file name
/// @param namelen length of file name
/// @param out pointer to receive the created inode
/// @return 0 on success (negitive indicates failure) 
int tempfs_create(INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type);

/// @brief read a directory and get its node contents
/// @param dir directory node
/// @param index n directory
/// @param result out results
/// @return 0
int tempfs_readdir(INode_t* dir, size_t index, INode_t** result);

/// @brief mount the root directory of the fs
/// @param root root node
/// @return success?
int tempfs_mount_root(INode_t** root);