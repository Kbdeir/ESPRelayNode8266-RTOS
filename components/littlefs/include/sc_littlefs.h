#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define SC_LFS_NAME_MAX 64
typedef struct { char name[SC_LFS_NAME_MAX]; uint32_t size; uint8_t is_dir; } sc_lfs_entry_t;
esp_err_t sc_lfs_mount(void);
int sc_lfs_read_file(const char *path, void *buf, size_t cap, size_t *out_len);
int sc_lfs_read_range(const char *path,size_t offset,void *buf,size_t cap,size_t *out_len);
int sc_lfs_write_file(const char *path,const void *buf,size_t len);
int sc_lfs_exists(const char *path);
int sc_lfs_remove(const char *path);
int sc_lfs_rename(const char *from,const char *to);
int sc_lfs_list_root(sc_lfs_entry_t *items,size_t cap,size_t *count);
int sc_lfs_count_root(size_t *count);
int sc_lfs_entry_at(size_t index,sc_lfs_entry_t *item);
size_t sc_lfs_total_bytes(void);
size_t sc_lfs_used_bytes(void);
int sc_lfs_read_stream_open(const char *path);
int sc_lfs_read_stream_read(void *buf,size_t cap,size_t *out_len);
int sc_lfs_read_stream_close(void);
int sc_lfs_stream_open(const char *path);
int sc_lfs_stream_write(const void *buf,size_t len);
int sc_lfs_stream_close(void);
