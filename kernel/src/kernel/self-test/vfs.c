#include <fs/vfs.h>
#include <stdio.h>
#include <string.h>
#include <panic.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>
#include <mm/heap.h>

#define TEST_DIR "/vfs_test"
#define TEST_FILE "test_file.txt"
#define TEST_CONTENT "Bleed Kernel VFS Self-Test"
#define TEMP_FILE "temp_drop.txt"
#define TEMP_CONTENT "Temporary drop test"

void vfs_test_self_test(void) {
    kprintf(LOG_INFO "VFS Self Test begin\n");

    // create test directory under vfs_root
    INode_t* test_dir_inode = NULL;
    path_t test_dir_path = vfs_path_from_abs(TEST_DIR);
    int r = vfs_create(&test_dir_path, &test_dir_inode, INODE_DIRECTORY);
    if (r < 0) ke_panic("VFS: failed to create test directory");

    // Persist directory for shell access
    test_dir_inode->shared = 1;
    kprintf(LOG_INFO "VFS: created directory %s\n", TEST_DIR);

    // create permanent test file
    char file_path_str[64];
    size_t dir_len = strlen(TEST_DIR);
    size_t file_len = strlen(TEST_FILE);
    for (size_t i = 0; i < dir_len; i++) file_path_str[i] = TEST_DIR[i];
    file_path_str[dir_len] = '/';
    for (size_t i = 0; i < file_len; i++) file_path_str[dir_len + 1 + i] = TEST_FILE[i];
    file_path_str[dir_len + 1 + file_len] = '\0';

    INode_t* test_file_inode = NULL;
    path_t file_path = vfs_path_from_abs(file_path_str);
    r = vfs_create(&file_path, &test_file_inode, INODE_FILE);
    if (r < 0) ke_panic("VFS: failed to create test file");

    size_t content_len = strlen(TEST_CONTENT);
    long written = inode_write(test_file_inode, TEST_CONTENT, content_len, 0);
    if ((size_t)written != content_len) ke_panic("VFS: write size mismatch");

    // Test filesize accuracy
    size_t reported_size = vfs_filesize(test_file_inode);
    if (reported_size != content_len) ke_panic("VFS: filesize mismatch for permanent file");
    kprintf(LOG_INFO "VFS: created permanent file %s with correct size %zu\n", file_path_str, reported_size);

    char temp_path_str[64];
    size_t temp_len = strlen(TEMP_FILE);
    for (size_t i = 0; i < dir_len; i++) temp_path_str[i] = TEST_DIR[i];
    temp_path_str[dir_len] = '/';
    for (size_t i = 0; i < temp_len; i++) temp_path_str[dir_len + 1 + i] = TEMP_FILE[i];
    temp_path_str[dir_len + 1 + temp_len] = '\0';

    INode_t* temp_inode = NULL;
    path_t temp_path = vfs_path_from_abs(temp_path_str);
    r = vfs_create(&temp_path, &temp_inode, INODE_FILE);
    if (r < 0) ke_panic("VFS: failed to create temp drop file");

    // write some content
    size_t temp_content_len = strlen(TEMP_CONTENT);
    written = inode_write(temp_inode, TEMP_CONTENT, temp_content_len, 0);
    if ((size_t)written != temp_content_len) ke_panic("VFS: temp file write size mismatch");

    // Test filesize accuracy for temp file
    reported_size = vfs_filesize(temp_inode);
    if (reported_size != temp_content_len) ke_panic("VFS: filesize mismatch for temp file");

    kprintf(LOG_INFO "VFS: created temporary file %s with correct size %zu\n", temp_path_str, reported_size);

    // Drop the temp file
    vfs_drop(temp_inode);
    if (temp_inode->shared != 0) ke_panic("VFS: temp file drop failed");

    kprintf(LOG_OK "Temporary drop test passed\n");
    kprintf(LOG_OK "VFS self-test PASSED\n");
}
