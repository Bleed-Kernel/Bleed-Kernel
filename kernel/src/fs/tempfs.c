#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>
#include <status.h>
#include <stdint.h>
#include <stddef.h>

#include "priv_tempfs.h"

/// @brief sanity check chunk pointers to make sure theyre not silly
/// @param p data pointer
/// @return success
static inline int tempfs_chunk_ptr_sane(const tempfs_data_t* p){
    uintptr_t v = (uintptr_t)p;
    if (!p) return 0;
    if (v & (sizeof(void*) - 1)) return 0;
    {
        uint64_t x = (uint64_t)v;
        uint64_t hi = x >> 48;
        if (!(hi == 0x0000ULL || hi == 0xFFFFULL))
            return 0;
    }
    return 1;
}

static INode_t **tempfs_entry_ptr(tempfs_INode_t *dir, size_t index) {
    if (!dir)
        return NULL;

    tempfs_data_t *chunk = dir->data;
    size_t idx = index;

    while (chunk && idx >= MAX_ENTRIES_PER_DATA_CHUNK) {
        idx -= MAX_ENTRIES_PER_DATA_CHUNK;
        chunk = chunk->next_chunk;
    }

    if (!chunk)
        return NULL;

    return &directory_entries(chunk)[idx];
}

/// @brief Create a new inode, store operations inside
/// @param type file or directory
/// @param ops operations pointer
/// @return new inode
INode_t* tempfs_create_inode(int type, const INodeOps_t* ops){
    INode_t* inode = kmalloc(sizeof(*inode));
    if (inode){
        memset(inode, 0, sizeof(*inode));   // ensure name is zero'd
        tempfs_INode_t* data = kmalloc(sizeof(*data));
        if (data){
            inode->type = type;
            inode->shared = 1;
            inode->ops = ops;
            inode->internal_data = data;

            data->capacity = 0;
            data->data = NULL;
        }else{
            kfree(inode, sizeof(*inode));
        }
    }

    return inode;
}

/// @brief release an inode's internal data (does not free the INode_t itself)
/// @param inode target inode
void tempfs_drop(INode_t* inode){
    tempfs_INode_t* tempfs_inode = inode->internal_data;
    tempfs_data_t* data = tempfs_inode->data;

    while(data){
        tempfs_data_t* next = data->next_chunk;
        kfree(data, TEMPFS_DATA_CHUNK_SIZE);
        data = next;
    }
    kfree(tempfs_inode, sizeof(tempfs_INode_t));
    inode->internal_data = NULL;
}

/// @brief truncate a file inode to new_size bytes, freeing excess chunks
/// @param inode target file inode
/// @param new_size new size in bytes
/// @return 0 on success, negative on failure
static int tempfs_truncate(INode_t* inode, size_t new_size){
    if (!inode || !inode->internal_data)
        return status_print_error(FILE_NOT_FOUND);

    tempfs_INode_t* tempfs_inode = inode->internal_data;

    if (new_size == tempfs_inode->capacity)
        return 0;

    if (new_size > tempfs_inode->capacity) {
        uint8_t z = 0;
        long r = tempfs_write(inode, &z, 1, new_size - 1);
        if (r < 0) return (int)r;
        tempfs_inode->capacity = new_size;
        return 0;
    }

    /* Shrink: free chunks beyond new_size and zero-fill the tail of the last kept chunk */
    size_t chunks_needed = (new_size + MAX_FILE_DATA_PER_CHUNK - 1) / MAX_FILE_DATA_PER_CHUNK;
    size_t tail_used     = new_size % MAX_FILE_DATA_PER_CHUNK;

    tempfs_data_t* chunk = tempfs_inode->data;
    tempfs_data_t** prev_next = &tempfs_inode->data;
    size_t chunk_idx = 0;

    while (chunk && chunk_idx < chunks_needed) {
        prev_next = &chunk->next_chunk;
        chunk = chunk->next_chunk;
        chunk_idx++;
    }

    /* Zero the unused tail bytes in the last kept chunk */
    if (chunks_needed > 0 && tail_used > 0 && *prev_next != chunk) {
        /* prev_next now points to the next_chunk field of the last kept chunk */
        tempfs_data_t* last = (tempfs_data_t*)((char*)prev_next - offsetof(tempfs_data_t, next_chunk));
        memset(file_data(last) + tail_used, 0, MAX_FILE_DATA_PER_CHUNK - tail_used);
    }

    /* Free all chunks past the new end */
    *prev_next = NULL;
    while (chunk) {
        tempfs_data_t* next = chunk->next_chunk;
        kfree(chunk, TEMPFS_DATA_CHUNK_SIZE);
        chunk = next;
    }

    tempfs_inode->capacity = new_size;
    return 0;
}

/// @brief create chunk of data and return its structure pointer
/// @return new structure pointer
tempfs_data_t* tempfs_new_data_chunk(){
    tempfs_data_t* data = kmalloc(TEMPFS_DATA_CHUNK_SIZE);
    if(data) memset(data, 0, TEMPFS_DATA_CHUNK_SIZE);
    return data;
}

/// @brief Look for an inode given as a child of an inode directory
/// @param dir parent
/// @param name name
/// @param namelen name length
/// @param result inode of result
/// @return success
int tempfs_lookup(INode_t* dir, const char* name, size_t namelen, INode_t** result){
    tempfs_INode_t* tempfs_inode = dir->internal_data;
    tempfs_data_t*  data = tempfs_inode->data;

    size_t remaining = tempfs_inode->capacity;

    while(data && remaining > 0){
        /* Only iterate the entries that are actually populated in this chunk */
        size_t in_chunk = remaining < MAX_ENTRIES_PER_DATA_CHUNK
                          ? remaining : MAX_ENTRIES_PER_DATA_CHUNK;

        for (size_t i = 0; i < in_chunk; i++){
            INode_t* child_node = directory_entries(data)[i];
            if (!child_node) continue;

            tempfs_INode_t* child_tempfs_node = child_node->internal_data;
            if (strlen(child_tempfs_node->name) == namelen &&
                memcmp(child_tempfs_node->name, name, namelen) == 0) {
                *result = child_node;
                return 0;
            }
        }
        remaining -= in_chunk;
        data = data->next_chunk;
    }
    return -1;  // file not found
}

/// @brief read an inodes data out to a pointer
/// @param inode inode to read
/// @param out_buffer output buffer
/// @param count size to read
/// @param offset read offset
/// @return read size (negitive indicates failure) 
long tempfs_read(INode_t* inode, void* out_buffer, size_t count, size_t offset){
    if (!inode || !inode->internal_data || !out_buffer)
        return status_print_error(FILE_NOT_FOUND);
    if (count == 0)
        return 0;

    tempfs_INode_t* tempfs_inode = inode->internal_data;
    tempfs_data_t* data = tempfs_inode->data;
    if (offset >= tempfs_inode->capacity) return 0; // EOF
    if (count > tempfs_inode->capacity - offset) count = tempfs_inode->capacity - offset;

    if (count > 0 && !data)
        return 0;

    {
        size_t max_hops = (tempfs_inode->capacity + MAX_FILE_DATA_PER_CHUNK - 1) / MAX_FILE_DATA_PER_CHUNK;
        size_t hops = 0;
        while(offset >= MAX_FILE_DATA_PER_CHUNK && data) {
            if (!tempfs_chunk_ptr_sane(data))
                return 0;
            data = data->next_chunk;
            offset -= MAX_FILE_DATA_PER_CHUNK;
            if (++hops > max_hops)
                return 0;
        }
    }

    size_t read_total = 0;
    size_t chunk_offset = offset;

    while(count > 0 && data) {
        if (!tempfs_chunk_ptr_sane(data))
            return (long)read_total;

        size_t chunk_avail = MAX_FILE_DATA_PER_CHUNK - chunk_offset;
        size_t to_read = count < chunk_avail ? count : chunk_avail;

        memcpy((char*)out_buffer + read_total, file_data(data) + chunk_offset, to_read);

        read_total += to_read;
        count -= to_read;

        data = data->next_chunk;
        chunk_offset = 0;
    }

    if (count > 0)
        return (long)read_total;
    
    return read_total;
}

/// @brief write to an inodes data
/// @param inode target inode
/// @param in_buffer data to write
/// @param count ammount of data to write
/// @param offset write offset in inode
/// @return write size (negitive indicates failure) 
long tempfs_write(INode_t* inode, const void* in_buffer, size_t count, size_t offset){
    if (!inode || !inode->internal_data || (!in_buffer && count > 0))
        return status_print_error(FILE_NOT_FOUND);
    if (count == 0)
        return 0;

    tempfs_INode_t* tempfs_inode = inode->internal_data;
    size_t start_offset = offset;
    tempfs_data_t** previous_next = &tempfs_inode->data;
    tempfs_data_t* data = tempfs_inode->data;

    if (offset > tempfs_inode->capacity) return status_print_error(OUT_OF_BOUNDS);

    // Skip chunks until we reach the right offset
    while (offset >= MAX_FILE_DATA_PER_CHUNK){
        if (data && !tempfs_chunk_ptr_sane(data))
            return status_print_error(FILE_NOT_FOUND);

        if (!data){
            data = *previous_next = tempfs_new_data_chunk();
            if (!data) return status_print_error(OUT_OF_MEMORY);
        }
        offset -= MAX_FILE_DATA_PER_CHUNK;
        previous_next = &data->next_chunk;
        data = data->next_chunk;
    }

    size_t written_total = 0;

    while (written_total < count){
        if (data && !tempfs_chunk_ptr_sane(data))
            return status_print_error(FILE_NOT_FOUND);

        if (!data){
            data = *previous_next = tempfs_new_data_chunk();
            if (!data) return status_print_error(OUT_OF_MEMORY);
        }

        size_t chunk_space = MAX_FILE_DATA_PER_CHUNK - offset;
        size_t write_now = (count - written_total < chunk_space) ? (count - written_total) : chunk_space;

        memcpy(file_data(data) + offset, (uint8_t*)in_buffer + written_total, write_now);

        written_total += write_now;
        offset = 0;
        previous_next = &data->next_chunk;
        data = data->next_chunk;
    }

    // Update inode capacity correctly
    if (tempfs_inode->capacity < start_offset + written_total)
        tempfs_inode->capacity = start_offset + written_total;

    return (long)written_total;
}

/// @brief Create a new file inside a directory
/// @param parent directory inode
/// @param name file name
/// @param namelen length of file name
/// @param out pointer to receive the created inode
/// @return 0 on success, negative on failure
int tempfs_create(INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type) {
    if (!parent || !parent->internal_data) {
        kprintf("tempfs_create: parent inode invalid!\n");
        return status_print_error(FILE_NOT_FOUND);
    }
    tempfs_INode_t* parent_data = parent->internal_data;
    size_t idx = parent_data->capacity;

    tempfs_data_t **prev_next = &parent_data->data, 
                  *chunk = parent_data->data;
    while(idx >= MAX_ENTRIES_PER_DATA_CHUNK) {
        if (!chunk) {
            chunk = *prev_next = tempfs_new_data_chunk();
            if (!chunk) return status_print_error(OUT_OF_MEMORY);
        }
        idx -= MAX_ENTRIES_PER_DATA_CHUNK;
        prev_next = &chunk->next_chunk;
        chunk = chunk->next_chunk;
    }
    if(!chunk) {
        *prev_next = chunk = tempfs_new_data_chunk();
        if(!chunk) return status_print_error(OUT_OF_MEMORY);
    }

    INode_t* file = node_type == INODE_DIRECTORY ? tempfs_create_inode(INODE_DIRECTORY, &dir_ops) : tempfs_create_inode(INODE_FILE, &file_ops);
    if (!file)
        return status_print_error(OUT_OF_MEMORY);
    tempfs_INode_t* file_int = file->internal_data;
    if (namelen >= TEMPFS_MAX_NAME_LEN) {
        tempfs_drop(file);
        kfree(file, sizeof(*file));
        return status_print_error(NAME_LIMITS);
    }
    memcpy(file_int->name, name, namelen);
    file_int->name[namelen] = '\0';

    directory_entries(chunk)[idx] = file;
    parent_data->capacity++;
    *result = file;
    return 0;
}

/// @brief read a directory and get its node contents
/// @param dir directory node
/// @param index n directory
/// @param result out results
/// @return 0
int tempfs_readdir(INode_t* dir, size_t index, INode_t** result){
    tempfs_INode_t* data = dir->internal_data;
    if (index >= data->capacity) return -FILE_NOT_FOUND;

    tempfs_data_t* chunk = data->data;
    size_t idx = index;

    while(chunk && idx >= MAX_ENTRIES_PER_DATA_CHUNK){
        idx -= MAX_ENTRIES_PER_DATA_CHUNK;
        chunk = chunk->next_chunk;
    }

    if (!chunk) return status_print_error(FILE_NOT_FOUND);

    *result = directory_entries(chunk)[idx];
    if (!*result) return status_print_error(FILE_NOT_FOUND);
    (*result)->shared++;

    return 0;
}

static int tempfs_unlink(INode_t* dir, const char* name, size_t namelen) {
    if (!dir || !dir->internal_data || !name || namelen == 0)
        return status_print_error(FILE_NOT_FOUND);

    tempfs_INode_t* dir_data = dir->internal_data;
    size_t size = dir_data->capacity;

    for (size_t i = 0; i < size; i++) {
        INode_t **slot = tempfs_entry_ptr(dir_data, i);
        if (!slot || !*slot)
            continue;

        INode_t *child = *slot;
        tempfs_INode_t *child_data = child->internal_data;
        if (!child_data)
            continue;

        if (strlen(child_data->name) == namelen && memcmp(child_data->name, name, namelen) == 0) {
            if (child->type == INODE_DIRECTORY) {
                tempfs_INode_t *child_dir = child->internal_data;
                if (child_dir && child_dir->capacity > 0)
                    return status_print_error(OUT_OF_BOUNDS);
            }

            size_t last_index = dir_data->capacity - 1;
            if (i != last_index) {
                INode_t **last_slot = tempfs_entry_ptr(dir_data, last_index);
                if (last_slot)
                    *slot = *last_slot;
            }
            {
                INode_t **last_slot = tempfs_entry_ptr(dir_data, last_index);
                if (last_slot)
                    *last_slot = NULL;
            }

            dir_data->capacity--;

            /*
             * Drop the tree's ref. If no fds are open (shared==1), vfs_drop
             * will free the inode. If fds are still open (shared==2+), it
             * stays alive until the last fd is closed.
             */
            extern void vfs_drop(INode_t*);
            vfs_drop(child);

            return 0;
        }
    }

    return status_print_error(FILE_NOT_FOUND);
}

static int tempfs_rename(INode_t* dir, const char* oldname, size_t oldlen, const char* newname, size_t newlen) {
    if (!dir || !dir->internal_data || !oldname || !newname || oldlen == 0 || newlen == 0)
        return status_print_error(FILE_NOT_FOUND);

    if (newlen >= TEMPFS_MAX_NAME_LEN)
        return status_print_error(NAME_LIMITS);

    tempfs_INode_t* dir_data = dir->internal_data;
    size_t size = dir_data->capacity;

    for (size_t i = 0; i < size; i++) {
        INode_t **slot = tempfs_entry_ptr(dir_data, i);
        if (!slot || !*slot)
            continue;

        tempfs_INode_t *child_data = (*slot)->internal_data;
        if (!child_data)
            continue;

        if (strlen(child_data->name) == newlen && memcmp(child_data->name, newname, newlen) == 0)
            return status_print_error(OUT_OF_BOUNDS);
    }

    for (size_t i = 0; i < size; i++) {
        INode_t **slot = tempfs_entry_ptr(dir_data, i);
        if (!slot || !*slot)
            continue;

        tempfs_INode_t *child_data = (*slot)->internal_data;
        if (!child_data)
            continue;

        if (strlen(child_data->name) == oldlen && memcmp(child_data->name, oldname, oldlen) == 0) {
            memcpy(child_data->name, newname, newlen);
            child_data->name[newlen] = '\0';
            return 0;
        }
    }

    return status_print_error(FILE_NOT_FOUND);
}

static size_t tempfs_size(INode_t* inode){
    if (!inode || !inode->internal_data)
        return 0;

    tempfs_INode_t* data = inode->internal_data;
    return data->capacity;
}


/// @brief mount the root directory of the fs
/// @param root root node
/// @return success?
int tempfs_mount_root(INode_t** root){
    return (*root = tempfs_create_inode(INODE_DIRECTORY, &dir_ops)) ? 0 : status_print_error(OUT_OF_MEMORY); // out of memory
}

const INodeOps_t dir_ops = {
    .readdir= tempfs_readdir,
    .create = tempfs_create,
    .lookup = tempfs_lookup,
    .unlink = tempfs_unlink,
    .rename = tempfs_rename,
    .drop   = tempfs_drop,
};

const INodeOps_t file_ops = {
    .read     = tempfs_read,
    .write    = tempfs_write,
    .truncate = tempfs_truncate,
    .drop     = tempfs_drop,
    .size     = tempfs_size
};

const filesystem tempfs = {
    .mount = tempfs_mount_root
};