#ifndef A20_NATIVE_FILE_CONTRACT_H
#define A20_NATIVE_FILE_CONTRACT_H
#include "liba20rt/a20_fs.h"
#include "liba20rt/a20_handle.h"
#include "liba20rt/a20_mem.h"
/* Same actual file operations on MMU servers and flat shared MCU kernels. */
/* A closed slot may immediately belong to another thread. Only a caller
 * with no concurrent handle allocation may check the just-closed number. */
static int native_file_contract(int serialized) {
#define F_CHECK(test, code)                                                    \
    do {                                                                       \
        if (!(test))                                                           \
            return code;                                                       \
    } while (0)
    a20_path_open_args_t open = {.dir = A20_HANDLE_NULL,
                                 .path = (uintptr_t)"/bin/INPUT.DAT",
                                 .path_len = 14};
    F_CHECK(a20_path_open(&open) == A20_OK &&
                open.out_handle != A20_HANDLE_NULL,
            1);
    a20_handle_t file = open.out_handle;
    a20_stat_t stat;
    F_CHECK(a20_hdl_stat(file, &stat) == A20_OK && stat.size_bytes == 2048, 2);
    uint64_t address;
    F_CHECK(a20_vm_alloc_pages(1, 3, &address) == A20_OK, 3);
    uint8_t *buffer = (void *)(uintptr_t)address;
    uint32_t checksum = 2166136261U;
    for (unsigned start = 0; start < 2048; start += 256) {
        a20_iovec_t iov[2] = {{address, 17}, {address + 17, 239}};
        uint64_t count;
        F_CHECK(a20_hdl_read(file, iov, 2, &count) == 256 && count == 256, 4);
        for (unsigned i = 0; i < 256; i++) {
            F_CHECK(buffer[i] == (uint8_t)((start + i) * 37 + 11), 5);
            checksum = (checksum ^ buffer[i]) * 16777619U;
        }
    }
    F_CHECK(checksum == 0x60f4edc5U, 6);
    uint64_t count;
    F_CHECK(a20_hdl_read_buf(file, buffer, 1, &count) == 0 && !count, 7);
    a20_off_t offset;
    F_CHECK(a20_hdl_seek(file, 487, 0, &offset) == A20_OK && offset == 487, 8);
    F_CHECK(a20_hdl_read_buf(file, buffer, 40, &count) == 40 && count == 40, 9);
    for (unsigned i = 0; i < 40; i++)
        F_CHECK(buffer[i] == (uint8_t)((487 + i) * 37 + 11), 10);
    F_CHECK(a20_hdl_close(file) == A20_OK, 11);
    F_CHECK(a20_hdl_read_buf(serialized ? file : A20_HANDLE_NULL, buffer, 1,
                             &count) == -A20_ERR_BAD_HANDLE,
            12);
    open.rights = A20_RIGHT_STAT;
    F_CHECK(a20_path_open(&open) == A20_OK, 13);
    F_CHECK(a20_hdl_read_buf(open.out_handle, buffer, 1, &count) ==
                -A20_ERR_ACCESS,
            14);
    F_CHECK(a20_hdl_close(open.out_handle) == A20_OK, 15);
    F_CHECK(a20_vm_unmap(address, 4096) == A20_OK, 16);
    open.path = (uintptr_t)"/bin/NOSUCH.DAT";
    open.path_len = 15;
    F_CHECK(a20_path_open(&open) == -A20_ERR_NOT_FOUND, 17);
    return 0;
#undef F_CHECK
}
/* Reopen beyond the descriptor ceiling on servers; callers choose the count
 * so the exact same algorithm also exercises small-memory targets. */
static int native_file_reopen(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        a20_path_open_args_t open = {.dir = A20_HANDLE_NULL,
                                     .path = (uintptr_t)"/bin/INPUT.DAT",
                                     .path_len = 14};
        if (a20_path_open(&open) != A20_OK ||
            open.out_handle == A20_HANDLE_NULL ||
            a20_hdl_close(open.out_handle) != A20_OK)
            return 1;
    }
    return 0;
}
#endif
