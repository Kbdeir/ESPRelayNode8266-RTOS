#include <string.h>
#include "lfs.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sc_littlefs.h"
static lfs_t lfs; static const esp_partition_t *part; static SemaphoreHandle_t mux;
static lfs_file_t reader; static int reader_open;
static lfs_file_t writer; static int writer_open;
static int rd(const struct lfs_config*c,lfs_block_t b,lfs_off_t o,void*buf,lfs_size_t n){return esp_partition_read(part,b*c->block_size+o,buf,n)==ESP_OK?0:LFS_ERR_IO;}
static int pr(const struct lfs_config*c,lfs_block_t b,lfs_off_t o,const void*buf,lfs_size_t n){return esp_partition_write(part,b*c->block_size+o,buf,n)==ESP_OK?0:LFS_ERR_IO;}
static int er(const struct lfs_config*c,lfs_block_t b){return esp_partition_erase_range(part,b*c->block_size,c->block_size)==ESP_OK?0:LFS_ERR_IO;}
static int sy(const struct lfs_config*c){return 0;}
static struct lfs_config cfg={.read=rd,.prog=pr,.erase=er,.sync=sy,.read_size=256,.prog_size=256,.block_size=4096,.cache_size=256,.lookahead_size=16,.block_cycles=500};
static void take(void){xSemaphoreTake(mux,portMAX_DELAY);} static void give(void){xSemaphoreGive(mux);}
esp_err_t sc_lfs_mount(void){mux=xSemaphoreCreateMutex();if(!mux)return ESP_ERR_NO_MEM;part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x40,"webfs");if(!part)return ESP_ERR_NOT_FOUND;cfg.block_count=part->size/cfg.block_size;int r=lfs_mount(&lfs,&cfg);if(r){if(lfs_format(&lfs,&cfg))return ESP_FAIL;if(lfs_mount(&lfs,&cfg))return ESP_FAIL;}return ESP_OK;}
int sc_lfs_exists(const char*p){take();struct lfs_info i;int r=lfs_stat(&lfs,p,&i)==0;give();return r;}
int sc_lfs_read_file(const char*p,void*b,size_t cap,size_t*out){take();lfs_file_t f;if(lfs_file_open(&lfs,&f,p,LFS_O_RDONLY)<0){give();return -1;}lfs_soff_t sz=lfs_file_size(&lfs,&f);if(sz<0||(size_t)sz>cap){lfs_file_close(&lfs,&f);give();return -2;}lfs_ssize_t n=lfs_file_read(&lfs,&f,b,sz);lfs_file_close(&lfs,&f);give();if(n<0)return -3;if(out)*out=n;return 0;}
int sc_lfs_read_range(const char*p,size_t off,void*b,size_t cap,size_t*out){take();lfs_file_t f;if(lfs_file_open(&lfs,&f,p,LFS_O_RDONLY)<0){give();return -1;}if(lfs_file_seek(&lfs,&f,(lfs_soff_t)off,LFS_SEEK_SET)<0){lfs_file_close(&lfs,&f);give();return -2;}lfs_ssize_t n=lfs_file_read(&lfs,&f,b,cap);lfs_file_close(&lfs,&f);give();if(n<0)return -3;if(out)*out=(size_t)n;return 0;}
int sc_lfs_write_file(const char*p,const void*b,size_t n){take();lfs_file_t f;if(lfs_file_open(&lfs,&f,p,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC)<0){give();return -1;}lfs_ssize_t w=lfs_file_write(&lfs,&f,b,n);lfs_file_close(&lfs,&f);give();return w==(lfs_ssize_t)n?0:-2;}
int sc_lfs_remove(const char*p){take();int r=lfs_remove(&lfs,p);give();return r;}
int sc_lfs_rename(const char*a,const char*b){take();int r=lfs_rename(&lfs,a,b);give();return r;}
int sc_lfs_list_root(sc_lfs_entry_t *items,size_t cap,size_t *count){if(!items||!count)return -1;take();lfs_dir_t d;if(lfs_dir_open(&lfs,&d,"/")<0){give();return -2;}size_t n=0;struct lfs_info i;while(n<cap&&lfs_dir_read(&lfs,&d,&i)>0){if(!strcmp(i.name,".")||!strcmp(i.name,".."))continue;size_t len=strnlen(i.name,sizeof(items[n].name)-2);items[n].name[0]='/';memcpy(items[n].name+1,i.name,len);items[n].name[len+1]=0;items[n].size=i.size;items[n].is_dir=i.type==LFS_TYPE_DIR;n++;}lfs_dir_close(&lfs,&d);give();*count=n;return 0;}
int sc_lfs_count_root(size_t *count){if(!count)return -1;take();lfs_dir_t d;if(lfs_dir_open(&lfs,&d,"/")<0){give();return -2;}size_t n=0;struct lfs_info i;int r;while((r=lfs_dir_read(&lfs,&d,&i))>0)if(strcmp(i.name,".")&&strcmp(i.name,".."))n++;lfs_dir_close(&lfs,&d);give();if(r<0)return -3;*count=n;return 0;}
int sc_lfs_entry_at(size_t index,sc_lfs_entry_t *item){if(!item)return -1;take();lfs_dir_t d;if(lfs_dir_open(&lfs,&d,"/")<0){give();return -2;}size_t n=0;struct lfs_info i;int r;while((r=lfs_dir_read(&lfs,&d,&i))>0){if(!strcmp(i.name,".")||!strcmp(i.name,".."))continue;if(n++!=index)continue;size_t len=strnlen(i.name,sizeof(item->name)-2);item->name[0]='/';memcpy(item->name+1,i.name,len);item->name[len+1]=0;item->size=i.size;item->is_dir=i.type==LFS_TYPE_DIR;lfs_dir_close(&lfs,&d);give();return 0;}lfs_dir_close(&lfs,&d);give();return r<0?-3:-4;}
size_t sc_lfs_total_bytes(void){return part?part->size:0;}
size_t sc_lfs_used_bytes(void){take();lfs_ssize_t blocks=lfs_fs_size(&lfs);give();return blocks>0?(size_t)blocks*cfg.block_size:0;}
int sc_lfs_read_stream_open(const char*p){take();if(reader_open){give();return -2;}int r=lfs_file_open(&lfs,&reader,p,LFS_O_RDONLY);if(r>=0)reader_open=1;give();return r;}
int sc_lfs_read_stream_read(void*b,size_t cap,size_t*out){if(!b||!out)return -1;take();if(!reader_open){give();return -2;}lfs_ssize_t n=lfs_file_read(&lfs,&reader,b,cap);give();if(n<0)return -3;*out=(size_t)n;return 0;}
int sc_lfs_read_stream_close(void){take();if(!reader_open){give();return -1;}int r=lfs_file_close(&lfs,&reader);reader_open=0;give();return r;}
int sc_lfs_stream_open(const char*p){take();if(writer_open){give();return -2;}int r=lfs_file_open(&lfs,&writer,p,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC);if(r>=0)writer_open=1;give();return r;}
int sc_lfs_stream_write(const void*b,size_t n){take();if(!writer_open){give();return -1;}lfs_ssize_t w=lfs_file_write(&lfs,&writer,b,n);give();return w==(lfs_ssize_t)n?0:-2;}
int sc_lfs_stream_close(void){take();if(!writer_open){give();return -1;}int r=lfs_file_close(&lfs,&writer);writer_open=0;give();return r;}
