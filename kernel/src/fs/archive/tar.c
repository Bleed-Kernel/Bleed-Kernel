#include <stdio.h>
#include <fs/vfs.h>
#include <ansii.h>
#include <stdint.h>
#include <fs/archive/tar.h>
#include <status.h>
#include <string.h>

#define TAR_BLOCK_SIZE  512
#ifndef SIZE_MAX
    #define SIZE_MAX ((size_t)-1)
#endif

/// @brief turns the octal value into the size of the tar file
/// @param str octal value
/// @param size header size
/// @return octal size_t
static size_t tar_octal_to_size(const char *octal_string, size_t header_size) {
    size_t result = 0;
    size_t i = 0;

    while (i < header_size && (octal_string[i] == ' ' || octal_string[i] == '\0')) i++;
    for (; i < header_size && octal_string[i] >= '0' && octal_string[i] <= '7'; i++) {
        result = (result << 3) + (octal_string[i] - '0');
    }

    return result;
}

/// @brief build the tars full path destination on the filesystem
/// @param out path abs
/// @param out_size out path size
/// @param prefix header prefix
/// @param name header name
static void tar_build_path(char* path_out, size_t path_out_size, const char* header_prefix, const char* header_name){
    memset(path_out, 0, path_out_size);
    size_t length = 0;

    if (header_prefix && header_prefix[0] != '\0'){
        for (size_t i = 0; i < 155 && header_prefix[i] != '\0'; i++){
            if (length < path_out_size - 1) path_out[length++] = header_prefix[i];
        }
        // Only add slash if prefix didnt end in one and we have room
        if (length > 0 && path_out[length-1] != '/' && length < path_out_size - 1) {
            path_out[length++] = '/';
        }
    }

    if (header_name){
        for (size_t i = 0; i < 100 && header_name[i] != '\0'; i++){
            if (length < path_out_size - 1) path_out[length++] = header_name[i];
        }
    }
    path_out[length] = '\0';
    
    if (length > 1 && path_out[length - 1] == '/') {
        path_out[length - 1] = '\0';
    }
}

/// @brief extract a tar files contents to the filesystem
/// @param tar_data .tar file data
/// @param tar_size .tar file size
/// @return success
int tar_extract(const void* tar_data, size_t tar_size){
    size_t offset = 0;

    while (offset + TAR_BLOCK_SIZE <= tar_size){
        tar_header_t* header = (tar_header_t*)((uint8_t*)tar_data + offset);

        if (header->name[0] == '\0')
            break;

        char full_path[256];
        tar_build_path(full_path, sizeof(full_path), header->prefix, header->name);

        size_t file_size = tar_octal_to_size(header->size, sizeof(header->size));
        bool is_dir = header->typeflag == '5';

        // Find last '/' in full_path
        size_t full_len = strlen(full_path);
        long last_slash = -1;
        for (long i = (long)full_len - 1; i >= 0; i--) {
            if (full_path[i] == '/') { last_slash = i; break; }
        }

        // walk it and create directories
        INode_t* parent = vfs_get_root();
        if (last_slash > 0) {
            // copy prefix into parent_prefix
            char parent_prefix[256];
            size_t plen = (size_t)last_slash;
            if (plen >= sizeof(parent_prefix)) plen = sizeof(parent_prefix) - 1;
            for (size_t i = 0; i < plen; i++) parent_prefix[i] = full_path[i];
            parent_prefix[plen] = '\0';

            // walk prefix components and create missing directories
            char component[128];
            size_t ci = 0;
            for (size_t i = 0;; i++) {
                char c = parent_prefix[i];

                if (c == '/' || c == '\0') {
                    if (ci > 0) {
                        component[ci] = '\0';

                        path_t comp_path = {
                            .root = parent,
                            .start = parent,
                            .data = component,
                            .data_length = ci
                        };

                        INode_t* next = NULL;
                        if (vfs_lookup(&comp_path, &next) < 0) {
                            // doesnt exist logic
                            if (vfs_create(&comp_path, &next, INODE_DIRECTORY) < 0) {
                                kprintf(LOG_ERROR "Tar: failed to create directory %s\n", component);
                                return status_print_error(TAR_EXTRACT_FAILURE);
                            }
                        }

                        if (parent) {
                            vfs_drop(parent);
                        }

                        parent = next;
                        ci = 0;
                    }

                    if (c == '\0') break;
                } else {
                    if (ci < sizeof(component) - 1) component[ci++] = c;
                }
            }
        }

        path_t final_path = vfs_path_from_abs(full_path);

        INode_t* inode = NULL;
        int res = vfs_lookup(&final_path, &inode);
        if (res < 0) {
            res = vfs_create(&final_path, &inode, (is_dir ? INODE_DIRECTORY : INODE_FILE));
        } else if (is_dir) {
            // Directory may already exist from parent-path creation.
            // Reusing it prevents duplicate directory entries.
            res = 0;
        } else {
            kprintf(LOG_ERROR "Tar extract failure: duplicate file %s (offset %lu)\n",
                    header->name, offset);
            return status_print_error(TAR_EXTRACT_FAILURE);
        }

        if (res < 0){
            kprintf(LOG_ERROR "Tar extract failure: %s (offset %lu)\n",
                    header->name, offset);
            return status_print_error(TAR_EXTRACT_FAILURE);
        }

        // Write file contents
        if (!is_dir && file_size > 0){
            if (offset > tar_size - TAR_BLOCK_SIZE) {
                kprintf(LOG_ERROR "Tar: invalid header offset for %s\n", full_path);
                return status_print_error(TAR_EXTRACT_FAILURE);
            }
            size_t content_offset = offset + TAR_BLOCK_SIZE;
            if (file_size > tar_size - content_offset) {
                kprintf(LOG_ERROR "Tar: truncated entry %s (size=%lu)\n", full_path, file_size);
                return status_print_error(TAR_EXTRACT_FAILURE);
            }
            if (inode_write(inode,
                            (uint8_t*)tar_data + content_offset,
                            file_size,
                            0) < 0) {
                kprintf(LOG_ERROR "Tar: write failed for %s\n", full_path);
                return status_print_error(TAR_EXTRACT_FAILURE);
            }
        }

        // Advance
        size_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        if (blocks > (SIZE_MAX - TAR_BLOCK_SIZE - offset) / TAR_BLOCK_SIZE) {
            if (inode) vfs_drop(inode); // Clean up before error out
            return status_print_error(TAR_EXTRACT_FAILURE);
        }
        size_t next_offset = offset + TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
        if (next_offset > tar_size) {
            if (inode) vfs_drop(inode); // Clean up before error
            return status_print_error(TAR_EXTRACT_FAILURE);
        }
        offset = next_offset;

        // Release the file and parent folder references
        if (inode) {
            vfs_drop(inode);
        }
        if (parent && parent != vfs_get_root()) {
            vfs_drop(parent);
        }
    }

    return 0;
}
