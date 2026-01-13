#include <fs/vfs.h>
#include <stdio.h>
#include <string.h>
#include <panic.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>
#include <mm/kalloc.h>

#define TEST_DIR "/vfs_test"
#define TEST_FILE "test_file.txt"
#define TEST_CONTENT "Bleed Kernel VFS Self-Test"
#define TEMP_FILE "temp_drop.txt"
#define TEMP_CONTENT "Temporary drop test"

void vfs_test_self_test(void) {
    serial_printf(LOG_INFO "VFS Self Test begin\n");

    INode_t* test_dir_inode = NULL;
    path_t test_dir_path = vfs_path_from_abs(TEST_DIR);
    int r = vfs_create(&test_dir_path, &test_dir_inode, INODE_DIRECTORY);
    if (r < 0) ke_panic("VFS: failed to create test directory");

    test_dir_inode->shared = 1;
    serial_printf(LOG_INFO "VFS: created directory %s\n", TEST_DIR);

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

    size_t reported_size = vfs_filesize(test_file_inode);
    if (reported_size != content_len) ke_panic("VFS: filesize mismatch for permanent file");
    serial_printf(LOG_INFO "VFS: created permanent file %s with correct size %u\n", file_path_str, reported_size);

    int fd = vfs_open(file_path_str, O_RDONLY);
    if (fd < 0) ke_panic("VFS: vfs_open failed on permanent file");

    char fd_buf[64];
    long rd = vfs_read(fd, fd_buf, sizeof(fd_buf) - 1);
    if (rd <= 0) ke_panic("VFS: vfs_read failed on permanent file");
    fd_buf[rd] = 0;

    if (memcmp(fd_buf, TEST_CONTENT, content_len) != 0)
        ke_panic("VFS: FD read content mismatch");

    if (vfs_close(fd) < 0) ke_panic("VFS: vfs_close failed");
    serial_printf(LOG_INFO "VFS: FD read test passed\n");

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

    size_t temp_content_len = strlen(TEMP_CONTENT);
    written = inode_write(temp_inode, TEMP_CONTENT, temp_content_len, 0);
    if ((size_t)written != temp_content_len) ke_panic("VFS: temp file write size mismatch");

    reported_size = vfs_filesize(temp_inode);
    if (reported_size != temp_content_len) ke_panic("VFS: filesize mismatch for temp file");
    serial_printf(LOG_INFO "VFS: created temporary file %s with correct size %u\n", temp_path_str, reported_size);

    int fdw = vfs_open(temp_path_str, O_RDWR);
    if (fdw < 0) ke_panic("VFS: vfs_open failed on temp file");

    const char *fd_msg = " FD_WRITE_OK";
    size_t fd_msg_len = strlen(fd_msg);
    long wr = vfs_write(fdw, fd_msg, fd_msg_len);
    if (wr != (long)fd_msg_len) ke_panic("VFS: vfs_write failed");

    // rewind
    current_fd_table->fds[fdw]->offset = 0;

    char fd_rw_buf[64];
    rd = vfs_read(fdw, fd_rw_buf, sizeof(fd_rw_buf) - 1);
    if (rd <= 0) ke_panic("VFS: FD readback failed");
    fd_rw_buf[rd] = 0;
    serial_printf(LOG_INFO "VFS: FD readback content: %s\n", fd_rw_buf);

    if (vfs_close(fdw) < 0) ke_panic("VFS: vfs_close failed on temp file");
    serial_printf(LOG_INFO "VFS: FD write/readback test passed\n");

    int fda = vfs_open(temp_path_str, O_WRONLY | O_APPEND);
    if (fda < 0) ke_panic("VFS: vfs_open failed for append");

    const char *append_msg = " APPEND_OK";
    if (vfs_write(fda, append_msg, strlen(append_msg)) <= 0)
        ke_panic("VFS: append write failed");

    if (vfs_close(fda) < 0) ke_panic("VFS: vfs_close failed on append");

    vfs_drop(temp_inode);
    temp_inode = NULL;

    serial_printf(LOG_OK "Temporary drop test passed\n");
    serial_printf(LOG_OK "VFS self-test PASSED\n");
}
