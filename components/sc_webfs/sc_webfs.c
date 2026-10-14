#include <string.h>
#include <stdio.h>
#include "sc_littlefs.h"
#include "sc_webfs.h"
static const char index_html[]="<!doctype html><meta http-equiv='refresh' content='0;url=/config.html'>";
extern const char sc_config_html[];extern const char sc_live_html[];extern const char sc_relay_html[];extern const char sc_inputs_html[];extern const char sc_temperature_html[];extern const char sc_timer_html[];extern const char sc_timer_saved_html[];extern const char sc_automation_html[];extern const char sc_files_html[];extern const char sc_backup_html[];extern const char sc_coredump_html[];extern const char sc_ota_html[];
static void seed(const char *p, const char *s)
{
    size_t len = strlen(s), off = 0, n = 0;
    char b[128];
    if (sc_lfs_exists(p)) {
        while (off < len) {
            size_t want = len - off < sizeof(b) ? len - off : sizeof(b);
            if (sc_lfs_read_range(p, off, b, want, &n) || n != want ||
                memcmp(b, s + off, n)) break;
            off += n;
        }
        if (off == len && sc_lfs_read_range(p, len, b, 1, &n) == 0 && n == 0)
            return;
    } else {
        char compressed[SC_LFS_NAME_MAX];
        int length = snprintf(compressed, sizeof(compressed), "%s.gz", p);
        // A compressed upload is already the recovery page's replacement.
        if (length > 0 && length < (int)sizeof(compressed) &&
            sc_lfs_exists(compressed)) return;
    }
    sc_lfs_write_file(p, s, len);
}
esp_err_t sc_webfs_init(void){esp_err_t e=sc_lfs_mount();if(e!=ESP_OK)return e;seed("/index.html",index_html);seed("/config.html",sc_config_html);seed("/LiveReadings.html",sc_live_html);seed("/RelayConfig.html",sc_relay_html);seed("/Input_Relays_Map.html",sc_inputs_html);seed("/AdvancedTemperatureConf.html",sc_temperature_html);seed("/Timer1.html",sc_timer_html);seed("/savetimer.html",sc_timer_saved_html);seed("/Automation.html",sc_automation_html);seed("/Files.html",sc_files_html);seed("/Backup.html",sc_backup_html);seed("/coredump.html",sc_coredump_html);seed("/FirmwareMaintenance.html",sc_ota_html);return ESP_OK;}
int sc_webfs_read_range(const char*p,size_t off,void*b,size_t cap,size_t*out){return sc_lfs_read_range(p,off,b,cap,out);}int sc_webfs_write(const char*p,const void*b,size_t n){return sc_lfs_write_file(p,b,n);}int sc_webfs_remove(const char*p){return sc_lfs_remove(p);}int sc_webfs_rename(const char*a,const char*b){return sc_lfs_rename(a,b);}int sc_webfs_list(sc_lfs_entry_t*i,size_t c,size_t*n){return sc_lfs_list_root(i,c,n);}int sc_webfs_count(size_t*n){return sc_lfs_count_root(n);}int sc_webfs_entry_at(size_t i,sc_lfs_entry_t*e){return sc_lfs_entry_at(i,e);}size_t sc_webfs_total(void){return sc_lfs_total_bytes();}size_t sc_webfs_used(void){return sc_lfs_used_bytes();}int sc_webfs_read_stream_open(const char*p){return sc_lfs_read_stream_open(p);}int sc_webfs_read_stream_read(void*b,size_t c,size_t*n){return sc_lfs_read_stream_read(b,c,n);}int sc_webfs_read_stream_close(void){return sc_lfs_read_stream_close();}int sc_webfs_stream_open(const char*p){return sc_lfs_stream_open(p);}int sc_webfs_stream_write(const void*b,size_t n){return sc_lfs_stream_write(b,n);}int sc_webfs_stream_close(void){return sc_lfs_stream_close();}
