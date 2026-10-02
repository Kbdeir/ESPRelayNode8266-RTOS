#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "sc_littlefs.h"
esp_err_t sc_webfs_init(void);
int sc_webfs_read_range(const char*path,size_t offset,void*buf,size_t cap,size_t*out_len);
int sc_webfs_write(const char*path,const void*buf,size_t len);
int sc_webfs_remove(const char*path);
int sc_webfs_rename(const char*from,const char*to);
int sc_webfs_list(sc_lfs_entry_t*items,size_t cap,size_t*count);
int sc_webfs_count(size_t*count);
int sc_webfs_entry_at(size_t index,sc_lfs_entry_t*item);
size_t sc_webfs_total(void);
size_t sc_webfs_used(void);
int sc_webfs_read_stream_open(const char*path);
int sc_webfs_read_stream_read(void*buf,size_t cap,size_t*out_len);
int sc_webfs_read_stream_close(void);
int sc_webfs_stream_open(const char*path);
int sc_webfs_stream_write(const void*buf,size_t len);
int sc_webfs_stream_close(void);
