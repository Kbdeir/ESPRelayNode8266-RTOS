#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "cJSON.h"
#include "sc_config.h"
#include "sc_health.h"
#include "sc_wifi.h"
#include "sc_relay.h"
#include "sc_mqtt.h"
#include "sc_webfs.h"
#include "sc_sensors.h"
#include "sc_inputs.h"
#include "sc_timer.h"
#include "sc_time.h"
#include "sc_diag.h"
#include "sc_ota.h"
#include "sc_ha.h"
#include "sc_automation.h"
#include "sc_modbus.h"
#include "sc_web.h"
#include "sc_format.h"

#define httpd_resp_sendstr(req, str) httpd_resp_send((req), (str), -1)

extern const char sc_timer_html[];
extern const char sc_timer_saved_html[];
static httpd_handle_t server;
typedef esp_err_t (*sc_web_handler_t)(httpd_req_t *r);
typedef struct {
  const char *uri;
  httpd_method_t method;
  sc_web_handler_t handler;
} sc_web_route_t;

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t *src, size_t len, char *out) {
  size_t i = 0, o = 0;
  while (i < len) {
    uint32_t v = (uint32_t)src[i++] << 16;
    bool second = i < len;
    if (second)
      v |= (uint32_t)src[i++] << 8;
    bool third = i < len;
    if (third)
      v |= src[i++];
    out[o++] = b64_table[(v >> 18) & 63];
    out[o++] = b64_table[(v >> 12) & 63];
    out[o++] = second ? b64_table[(v >> 6) & 63] : '=';
    out[o++] = third ? b64_table[v & 63] : '=';
  }
  return o;
}

static void reboot_task(void*p){(void)p;vTaskDelay(pdMS_TO_TICKS(1000));esp_restart();}
static void schedule_reboot(void){if(xTaskCreate(reboot_task,"config_reboot",1024,NULL,2,NULL)!=pdPASS)esp_restart();}
static esp_err_t restart_device(httpd_req_t*r){httpd_resp_set_type(r,"application/json");esp_err_t e=httpd_resp_sendstr(r,"{\"ok\":true,\"message\":\"Restarting\"}");schedule_reboot();return e;}
static esp_err_t send_error(httpd_req_t*r,const char*status,const char*msg){httpd_resp_set_status(r,status);httpd_resp_set_type(r,"text/plain");return httpd_resp_sendstr(r,msg?msg:"error");}
static bool request_authorized(httpd_req_t *r) {
  char user[SC_USER_LEN], password[SC_PASS_LEN];
  const sc_config_t *cfg = sc_config_acquire();
  snprintf(user, sizeof(user), "%s", cfg->web_user);
  snprintf(password, sizeof(password), "%s", cfg->web_password);
  sc_config_release();

  char plain[SC_USER_LEN + SC_PASS_LEN + 1];
  int plain_len = snprintf(plain, sizeof(plain), "%s:%s", user, password);
  if (plain_len < 0 || plain_len >= (int)sizeof(plain))
    return false;
  char expected[((SC_USER_LEN + SC_PASS_LEN + 3) / 3) * 4 + 1];
  size_t expected_len = b64_encode((const uint8_t *)plain, plain_len, expected);
  expected[expected_len] = 0;

  char header[sizeof(expected) + 7];
  size_t header_len = httpd_req_get_hdr_value_len(r, "Authorization");
  if (!header_len || header_len >= sizeof(header) ||
      httpd_req_get_hdr_value_str(r, "Authorization", header,
                                  sizeof(header)) != ESP_OK)
    return false;
  return !strncasecmp(header, "Basic ", 6) &&
         strlen(header + 6) == expected_len &&
         !memcmp(header + 6, expected, expected_len);
}
static esp_err_t auth_dispatch(httpd_req_t *r) {
  const sc_web_route_t *route = (const sc_web_route_t *)r->user_ctx;
  if (!request_authorized(r)) {
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "WWW-Authenticate",
                       "Basic realm=\"ESP Relay Node\", charset=\"UTF-8\"");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Connection", "close");
    return httpd_resp_sendstr(r, "Authentication required");
  }
  return route && route->handler ? route->handler(r) : ESP_FAIL;
}
static esp_err_t json_text(httpd_req_t*r,const char*body){httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");return httpd_resp_sendstr(r,body);}
typedef struct {
  httpd_req_t *req;
  char data[128];
  size_t used;
  esp_err_t error;
} json_output_t;

static void json_flush(json_output_t *out) {
  if (out->error == ESP_OK && out->used)
    out->error = httpd_resp_send_chunk(out->req, out->data, out->used);
  out->used = 0;
}
static void json_write(json_output_t *out, const char *data, size_t size) {
  while (size && out->error == ESP_OK) {
    size_t room = sizeof(out->data) - out->used;
    size_t take = size < room ? size : room;
    memcpy(out->data + out->used, data, take);
    out->used += take;
    data += take;
    size -= take;
    if (out->used == sizeof(out->data)) json_flush(out);
  }
}
static void json_write_text(json_output_t *out, const char *text) {
  json_write(out, text, strlen(text));
}
static void json_write_string(json_output_t *out, const char *value) {
  static const char hex[] = "0123456789abcdef";
  json_write_text(out, "\"");
  for (; value && *value && out->error == ESP_OK; value++) {
    unsigned char c = (unsigned char)*value;
    if (c == '"' || c == '\\') {
      char escape[2] = {'\\', (char)c};
      json_write(out, escape, sizeof(escape));
    } else if (c < 0x20) {
      char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
      json_write(out, escape, sizeof(escape));
    } else json_write(out, (const char *)&c, 1);
  }
  json_write_text(out, "\"");
}
static void json_write_field(json_output_t *out, const char *key, const char *value) {
  json_write_text(out, ",");
  json_write_string(out, key);
  json_write_text(out, ":");
  json_write_string(out, value);
}
static esp_err_t json_finish(json_output_t *out) {
  json_flush(out);
  return out->error == ESP_OK ? httpd_resp_send_chunk(out->req, NULL, 0) : out->error;
}
static void json_escape(char*out,size_t cap,const char*in){size_t o=0;if(!cap)return;for(;in&&*in&&o+1<cap;in++){unsigned char c=(unsigned char)*in;if(c=='"'||c=='\\'){if(o+2>=cap)break;out[o++]='\\';out[o++]=(char)c;}else if(c<0x20){if(o+6>=cap)break;int n=snprintf(out+o,cap-o,"\\u%04x",c);if(n!=6)break;o+=6;}else out[o++]=(char)c;}out[o]=0;}
static bool json_appendf(char*out,size_t cap,size_t*off,const char*fmt,...){if(*off>=cap)return false;va_list ap;va_start(ap,fmt);int n=vsnprintf(out+*off,cap-*off,fmt,ap);va_end(ap);if(n<0||(size_t)n>=cap-*off){*off=cap;return false;}*off+=(size_t)n;return true;}
static bool json_string_field(char*out,size_t cap,size_t*off,const char*key,const char*value){const char*comma=*off&&out[*off-1]!='{'?",":"";if(!json_appendf(out,cap,off,"%s\"%s\":\"",comma,key))return false;for(;value&&*value;value++){unsigned char c=(unsigned char)*value;if(c=='"'||c=='\\'){if(!json_appendf(out,cap,off,"\\%c",c))return false;}else if(c<0x20){if(!json_appendf(out,cap,off,"\\u%04x",c))return false;}else{if(*off+1>=cap){*off=cap;return false;}out[(*off)++]=(char)c;out[*off]=0;}}return json_appendf(out,cap,off,"\"");}
static esp_err_t js(httpd_req_t*r,cJSON*j){char*s=cJSON_PrintUnformatted(j);httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");esp_err_t e=httpd_resp_sendstr(r,s?s:"{}");free(s);cJSON_Delete(j);return e;}
static int hex_value(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static void form_url_decode(char*s){char*read=s,*write=s;while(*read){if(*read=='+'){*write++=' ';read++;continue;}if(*read=='%'&&read[1]&&read[2]){int high=hex_value(read[1]),low=hex_value(read[2]);if(high>=0&&low>=0){*write++=(char)((high<<4)|low);read+=3;continue;}}*write++=*read++;}*write=0;}
static void qstr(const char*q,const char*key,char*dst,size_t cap){char t[320];if(cap&&httpd_query_key_value(q,key,t,sizeof(t))==ESP_OK){form_url_decode(t);size_t n=strlen(t);if(n>=cap)n=cap-1;memcpy(dst,t,n);dst[n]=0;}}static int qint(const char*q,const char*key,int cur){char t[24];return httpd_query_key_value(q,key,t,sizeof(t))==ESP_OK?atoi(t):cur;}static uint32_t qu32(const char*q,const char*key,uint32_t cur){char t[24],*end=NULL;if(httpd_query_key_value(q,key,t,sizeof(t))!=ESP_OK)return cur;if(t[0]=='-')return 0;unsigned long long value=strtoull(t,&end,10);if(!t[0]||!end||*end)return cur;return value>UINT32_MAX?UINT32_MAX:(uint32_t)value;}static float qfloat(const char*q,const char*key,float cur){char t[32];return httpd_query_key_value(q,key,t,sizeof(t))==ESP_OK?(float)atof(t):cur;}
static bool qcondition_value(const char*q,const char*key,float*out){char t[32];if(!out||httpd_query_key_value(q,key,t,sizeof(t))!=ESP_OK)return false;form_url_decode(t);char*start=t;while(isspace((unsigned char)*start))start++;char*last=start+strlen(start);while(last>start&&isspace((unsigned char)last[-1]))*--last=0;if(!strcasecmp(start,"on")||!strcasecmp(start,"true")||!strcasecmp(start,"yes")){*out=1.0f;return true;}if(!strcasecmp(start,"off")||!strcasecmp(start,"false")||!strcasecmp(start,"no")){*out=0.0f;return true;}char*end=NULL;float value=strtof(start,&end);if(!start[0]||end==start||*end||!isfinite(value))return false;*out=value;return true;}
static bool query_has(const char*q,const char*key);
static bool valid_publish_topic(const char *topic) {
  return topic && topic[0] && strlen(topic) < SC_TOPIC_LEN &&
         !strchr(topic, '#') && !strchr(topic, '+');
}
static bool valid_modbus_host(const char *host) {
  struct in_addr address;
  return host && inet_aton(host, &address) &&
         address.s_addr != INADDR_ANY &&
         address.s_addr != INADDR_BROADCAST &&
         (ntohl(address.s_addr) >> 24) != 127;
}
static bool valid_modbus_settings(const char *host,
                                  const sc_remote_modbus_config_t *m) {
  return valid_modbus_host(host) && m->port && m->unit >= 1 && m->unit <= 247 &&
         m->function >= 1 && m->function <= 4 &&
         m->value_type <= SC_MODBUS_F32 && m->word_order <= 3 &&
         m->poll_sec >= 5 && m->poll_sec <= SC_REMOTE_STALE_SEC &&
         isfinite(m->scale) && isfinite(m->offset) &&
         (m->function > 2 || m->value_type == SC_MODBUS_BOOL) &&
         (m->function <= 2 || m->value_type != SC_MODBUS_BOOL) &&
         (m->function <= 2 ||
          (uint32_t)m->address +
          (m->value_type >= SC_MODBUS_U32 ? 2U : 1U) <= 65536U);
}
static bool safe_path(const char*p){return p&&p[0]=='/'&&p[1]!='.'&&!strstr(p,"..")&&strlen(p)<SC_LFS_NAME_MAX;}
static esp_err_t recv_body(httpd_req_t*r,char*b,size_t cap){if(r->content_len<=0||r->content_len>=(int)cap){send_error(r,HTTPD_400,"invalid body size");return ESP_FAIL;}int got=0;while(got<r->content_len){int n=httpd_req_recv(r,b+got,r->content_len-got);if(n<=0)return ESP_FAIL;got+=n;}b[got]=0;return ESP_OK;}
static void controller_id(char out[13]){uint8_t mac[6]={0};if(esp_wifi_get_mac(ESP_IF_WIFI_STA,mac)!=ESP_OK){snprintf(out,13,"ESP8266");return;}snprintf(out,13,"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);}
static void uptime_string(char*out,size_t cap){uint32_t sec=(uint32_t)(esp_timer_get_time()/1000000ULL);uint32_t days=sec/86400;sec%=86400;snprintf(out,cap,"%ud %02u:%02u:%02u",(unsigned)days,(unsigned)(sec/3600),(unsigned)((sec%3600)/60),(unsigned)(sec%60));}

static esp_err_t heap(httpd_req_t *r) {
  sc_health_http_request();
  sc_health_snapshot_t h;
  sc_health_snapshot(&h);
  char body[256];
  int n = snprintf(body, sizeof(body),
    "{\"heap\":%u,\"heapMin\":%u,\"heapMaxBlk\":%u,\"wifiReconnects\":%u,"
    "\"mqttReconnects\":%u,\"httpStackFree\":%u,\"mqttPubStackFree\":%u}",
    (unsigned)h.free_heap, (unsigned)h.min_free_heap, (unsigned)h.max_free_block,
    (unsigned)h.wifi_reconnects, (unsigned)h.mqtt_reconnects,
    (unsigned)uxTaskGetStackHighWaterMark(NULL),
    (unsigned)sc_mqtt_publisher_stack_free());
  if (n < 0 || n >= (int)sizeof(body)) return ESP_FAIL;
  return json_text(r, body);
}
static esp_err_t getcfg(httpd_req_t *r) {
  int relay = 0;
  char query[96], value[8];
  if (httpd_req_get_url_query_str(r, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "relay", value, sizeof(value)) == ESP_OK)
    relay = atoi(value);

  char node_id[13], system_time[40], uptime[32], current[32], body[3072];
  controller_id(node_id);
  sc_time_format(system_time, sizeof(system_time));
  uptime_string(uptime, sizeof(uptime));

  const sc_config_t *c = sc_config_acquire();
  if (relay < 0 || relay >= c->relay_count)
    relay = 0;
  if (!sc_format_fixed(current, sizeof(current), c->relay[relay].max_current, 3)) {
    sc_config_release();
    return send_error(r, HTTPD_500, "invalid current limit");
  }
  size_t offset = 0;
  bool ok =
      json_appendf(body, sizeof(body), &offset, "{") &&
      json_string_field(body, sizeof(body), &offset, "NODEID", node_id) &&
      json_string_field(body, sizeof(body), &offset, "PhyLoc", c->phy_loc) &&
      json_string_field(body, sizeof(body), &offset, "ssid", c->ssid) &&
      json_string_field(body, sizeof(body), &offset, "pass", "") &&
      json_string_field(body, sizeof(body), &offset, "timeserver", c->timeserver) &&
      json_string_field(body, sizeof(body), &offset, "Web_User", c->web_user) &&
      json_string_field(body, sizeof(body), &offset, "Web_Password", "") &&
      json_appendf(body, sizeof(body), &offset,
                   ",\"ntptz\":%d,\"MQTT_Active\":%u,\"HA_Discovery_Active\":%u",
                   (int)c->timezone_hours, (unsigned)c->mqtt_active,
                   (unsigned)c->ha_discovery_active) &&
      json_string_field(body, sizeof(body), &offset, "MQTT_BROKER",
                        c->mqtt_broker) &&
      json_appendf(body, sizeof(body), &offset,
                   ",\"MQTT_B_PRT\":%u,\"MQTT_KeepAliveSeconds\":%u",
                   (unsigned)c->mqtt_port,
                   (unsigned)c->mqtt_keepalive_sec) &&
      json_string_field(body, sizeof(body), &offset, "mqttUser", c->mqtt_user) &&
      json_string_field(body, sizeof(body), &offset, "mqttPass", "") &&
      json_appendf(body, sizeof(body), &offset,
                   ",\"Modbus_Active\":%u,\"Modbus_Port\":%u,"
                   "\"Modbus_Slave_ID\":%u,\"DS18B20_Active\":%u,"
                   "\"TempSensorPin\":%u,\"SecondTempSensorPin\":%u",
                   (unsigned)c->modbus_active, (unsigned)c->modbus_port,
                   (unsigned)c->modbus_unit_id,
                   (unsigned)c->ds18b20_active,
                   (unsigned)c->ds18b20_gpio,
                   (unsigned)c->ds18b20_second_gpio) &&
      json_string_field(body, sizeof(body), &offset, "systemtime", system_time) &&
      json_string_field(body, sizeof(body), &offset, "uptime", uptime) &&
      json_appendf(body, sizeof(body), &offset, ",\"heap\":%u,\"RELAYNB\":%d",
                   (unsigned)esp_get_free_heap_size(), relay) &&
      json_string_field(body, sizeof(body), &offset, "PUB_TOPIC1",
                        c->relay[relay].pub_topic) &&
      json_string_field(body, sizeof(body), &offset, "STATE_PUB_TOPIC",
                        c->relay[relay].state_pub_topic) &&
      json_string_field(body, sizeof(body), &offset, "SUB_TOPIC1",
                        c->relay[relay].sub_topic) &&
      json_string_field(body, sizeof(body), &offset, "LWILL_TOPIC",
                        c->relay[relay].lwill_topic) &&
      json_string_field(body, sizeof(body), &offset, "ACS_AMPS",
                        c->relay[relay].acs_topic) &&
      json_appendf(body, sizeof(body), &offset,
                   ",\"ttl\":%u,\"tta\":%u,\"ACS_Active\":%u,"
                   "\"ACS_Sensor_Model\":%u,\"Max_Current\":%s,"
                   "\"%s\":\"%s\"}",
                   (unsigned)c->relay[relay].ttl_sec,
                   (unsigned)c->relay[relay].tta_sec,
                   (unsigned)c->relay[relay].acs_enabled,
                   (unsigned)c->relay[relay].acs_model_amps,
                   current,
                   relay == 0 ? "RSTATE0" : "RSTATE1",
                   sc_relay_get(relay) ? "ON" : "OFF");
  sc_config_release();
  if (!ok)
    return send_error(r, HTTPD_500, "configuration response too large");
  return json_text(r, body);
}
static esp_err_t sensors(httpd_req_t*r){sc_sensor_snapshot_t s;sc_sensors_snapshot(&s);sc_health_snapshot_t h;sc_health_snapshot(&h);char temp1[24],temp2[24],acs1[24],relays[128]={0};if(s.temp1_valid)sc_format_fixed(temp1,sizeof(temp1),s.temp1_c,2);else snprintf(temp1,sizeof(temp1),"\"-\"");if(s.temp2_valid)sc_format_fixed(temp2,sizeof(temp2),s.temp2_c,2);else snprintf(temp2,sizeof(temp2),"\"-\"");if(s.acs_valid)sc_format_fixed(acs1,sizeof(acs1),s.acs_amps,3);else snprintf(acs1,sizeof(acs1),"\"-\"");size_t roff=0;for(uint8_t i=0;i<sc_relay_count()&&roff<sizeof(relays);i++)roff+=(size_t)snprintf(relays+roff,sizeof(relays)-roff,"%s{\"nb\":%u,\"state\":\"%s\"}",i?",":"",(unsigned)i,sc_relay_get(i)?"ON":"OFF");char tm[40],up[32],body[768];sc_time_format(tm,sizeof(tm));uptime_string(up,sizeof(up));int n=snprintf(body,sizeof(body),"{\"ATEMP1\":%s,\"ATEMP2\":%s,\"ACS1\":%s,\"ACS2\":\"-\",\"CT_VOLTAGE\":\"NA\",\"CT_CURRENT\":\"NA\",\"CT_POWER\":\"NA\",\"CT_PF\":\"NA\",\"CT_WH\":\"NA\",\"CT_MTD_WH\":\"NA\",\"CT_YTD_WH\":\"NA\",\"relays\":[%s],\"RSTATE0\":\"%s\",\"RSTATE1\":\"%s\",\"time\":\"%s\",\"uptime\":\"%s\",\"heap\":%u,\"heapMin\":%u,\"heapMaxBlk\":%u}",temp1,temp2,acs1,relays,sc_relay_get(0)?"ON":"OFF",sc_relay_get(1)?"ON":"OFF",tm,up,(unsigned)h.free_heap,(unsigned)h.min_free_heap,(unsigned)h.max_free_block);if(n<0||n>=(int)sizeof(body))return send_error(r,HTTPD_500,"sensor response too large");return json_text(r,body);}
static esp_err_t feature_flags(httpd_req_t*r){return json_text(r,"{\"waterFlowSensor\":false,\"oled1306\":false,\"emonlib\":false,\"solarHeater\":false,\"ads1x15\":false,\"hst\":false,\"pressureSensor\":false,\"adsDCCurrent\":false}");}
static esp_err_t wifi_status(httpd_req_t*r){sc_wifi_status_t s;sc_wifi_status(&s);char ssid[199],body[512];json_escape(ssid,sizeof(ssid),s.ssid);int n=snprintf(body,sizeof(body),"{\"connected\":%s,\"ip\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d}",s.connected?"true":"false",s.ip,s.gateway,s.dns1,s.dns2,ssid,(int)s.rssi);if(n<0||n>=(int)sizeof(body))return send_error(r,HTTPD_500,"Wi-Fi response too large");return json_text(r,body);}
static esp_err_t mqtt_status(httpd_req_t*r){sc_mqtt_status_t s;sc_mqtt_status(&s);char server_name[88],escaped_server[529],escaped_health[577],body[1408];snprintf(server_name,sizeof(server_name),"%s:%u",s.server,(unsigned)s.port);json_escape(escaped_server,sizeof(escaped_server),server_name);json_escape(escaped_health,sizeof(escaped_health),s.health_topic);int n=snprintf(body,sizeof(body),"{\"connected\":%s,\"enabled\":%s,\"status\":\"%s\",\"server\":\"%s\",\"port\":%u,\"route\":\"direct\",\"healthTopic\":\"%s\"}",s.connected?"true":"false",s.enabled?"true":"false",!s.enabled?"Disabled":(s.connected?"Connected":"Disconnected"),escaped_server,(unsigned)s.port,escaped_health);if(n<0||n>=(int)sizeof(body))return send_error(r,HTTPD_500,"MQTT response too large");return json_text(r,body);}
static esp_err_t wireguard_status(httpd_req_t*r){return json_text(r,"{\"enabled\":false,\"started\":false,\"connected\":false,\"networkUp\":false,\"reconnecting\":false,\"address\":\"\",\"endpoint\":\"\",\"resolvedEndpoint\":\"\",\"peerIp\":\"\",\"port\":0,\"dns\":\"\"}");}
static esp_err_t relayctl(httpd_req_t*r){char q[128],v[16];int rn=0;if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return send_error(r,HTTPD_400,"missing query");if(httpd_query_key_value(q,"relay",v,sizeof(v))==ESP_OK)rn=atoi(v);if(rn<0||rn>=sc_relay_count())return send_error(r,HTTPD_400,"invalid relay");if(httpd_query_key_value(q,"action",v,sizeof(v))!=ESP_OK&&httpd_query_key_value(q,"state",v,sizeof(v))!=ESP_OK)return send_error(r,HTTPD_400,"missing action");sc_relay_cmd_t c={.relay=rn};const char*accepted;if(!strcasecmp(v,"toggle")||!strcasecmp(v,"tog")){c.op=SC_RELAY_TOGGLE;accepted="TOGGLE";}else if(!strcasecmp(v,"on")||!strcmp(v,"1")){c.op=SC_RELAY_SET;c.value=true;accepted="ON";}else if(!strcasecmp(v,"off")||!strcmp(v,"0")){c.op=SC_RELAY_SET;c.value=false;accepted="OFF";}else return send_error(r,HTTPD_400,"invalid action");if(!sc_relay_submit(&c))return send_error(r,"503 Service Unavailable","queue full");httpd_resp_set_hdr(r,"Connection","close");cJSON*j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);cJSON_AddNumberToObject(j,"relay",rn);cJSON_AddStringToObject(j,"accepted",accepted);cJSON_AddStringToObject(j,"state",sc_relay_get(rn)?"ON":"OFF");return js(r,j);}
static esp_err_t relaystate(httpd_req_t*r){char q[64],v[8],body[160];int n=0;if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK&&httpd_query_key_value(q,"relay",v,sizeof(v))==ESP_OK)n=atoi(v);sc_relay_status_t status;if(n<0||!sc_relay_status((uint8_t)n,&status))return json_text(r,"{\"relay\":-1,\"state\":\"NA\",\"ttlActive\":false,\"ttaActive\":false,\"ttlRemaining\":0,\"ttaRemaining\":0}");snprintf(body,sizeof(body),"{\"relay\":%d,\"state\":\"%s\",\"ttlActive\":%s,\"ttaActive\":%s,\"ttlRemaining\":%u,\"ttaRemaining\":%u}",n,status.state?"ON":"OFF",status.ttl_active?"true":"false",status.tta_active?"true":"false",(unsigned)status.ttl_remaining_sec,(unsigned)status.tta_remaining_sec);return json_text(r,body);}

static bool automation_source_valid(int source)
{
  return source >= SC_AUTOMATION_SOURCE_NONE &&
         (source <= SC_AUTOMATION_SOURCE_RELAY4 ||
          (source >= SC_AUTOMATION_SOURCE_REMOTE1 &&
           source <= SC_AUTOMATION_SOURCE_REMOTE8));
}

static esp_err_t automation_get(httpd_req_t *r)
{
  const sc_config_t *config = sc_config_acquire();
  uint8_t relay_count = config->relay_count;
  sc_config_release();
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_hdr(r, "Connection", "close");
  char chunk[1024];
  int n = snprintf(chunk, sizeof(chunk),
                   "{\"relayCount\":%u,\"staleSeconds\":%u,\"rules\":[",
                   (unsigned)relay_count, (unsigned)SC_REMOTE_STALE_SEC);
  if (n < 0 || n >= (int)sizeof(chunk) ||
      httpd_resp_send_chunk(r, chunk, n) != ESP_OK) return ESP_FAIL;
  for (uint8_t i = 0; i < SC_MAX_AUTOMATION_RULES; i++) {
    sc_automation_rule_t rule_value;
    sc_modbus_rule_target_t target_value;
    config = sc_config_acquire();
    rule_value = config->automation[i];
    target_value = config->modbus_rule[i];
    sc_config_release();
    const sc_automation_rule_t *rule = &rule_value;
    const sc_modbus_rule_target_t *target = &target_value;
    char host[16] = "";
    if (target->ipv4) {
      struct in_addr address = {.s_addr = target->ipv4};
      snprintf(host, sizeof(host), "%s", inet_ntoa(address));
    }
    sc_automation_status_t status;
    sc_automation_status(i, &status);
    n = snprintf(chunk, sizeof(chunk),
      "%s{\"id\":%u,\"enabled\":%s,\"target\":%u,\"action\":%u,"
      "\"logic\":%u,\"trigger\":%u,\"gate\":%u,\"hold\":%u,"
      "\"cooldown\":%u,\"output\":%u,\"mbHost\":\"%s\",\"mbPort\":%u,"
      "\"mbUnit\":%u,\"mbAddress\":%u,\"mbClear\":%u,"
      "\"conditions\":[{\"source\":%u,\"op\":%u,"
      "\"value1\":%.6g,\"value2\":%.6g},{\"source\":%u,\"op\":%u,"
      "\"value1\":%.6g,\"value2\":%.6g}],\"status\":{"
      "\"available\":%s,\"gateOpen\":%s,\"matched\":%s,"
      "\"stableSeconds\":%u,\"secondsSinceFire\":%u}}",
      i ? "," : "", (unsigned)(i + 1), rule->enabled ? "true" : "false",
      (unsigned)rule->target_relay, (unsigned)rule->action,
      (unsigned)rule->logic, (unsigned)rule->trigger_mode,
      (unsigned)rule->gate_timer, (unsigned)rule->hold_sec,
      (unsigned)rule->cooldown_sec,
      (unsigned)target->remote, host, (unsigned)target->port,
      (unsigned)target->unit, (unsigned)target->address,
      (unsigned)target->send_clear,
      (unsigned)rule->condition[0].source, (unsigned)rule->condition[0].op,
      (double)rule->condition[0].value1, (double)rule->condition[0].value2,
      (unsigned)rule->condition[1].source, (unsigned)rule->condition[1].op,
      (double)rule->condition[1].value1, (double)rule->condition[1].value2,
      status.source_available ? "true" : "false",
      status.gate_open ? "true" : "false",
      status.matched ? "true" : "false",
      (unsigned)status.stable_sec, (unsigned)status.seconds_since_fire);
    if (n < 0 || n >= (int)sizeof(chunk) ||
        httpd_resp_send_chunk(r, chunk, n) != ESP_OK) return ESP_FAIL;
  }
  if (httpd_resp_send_chunk(r, "],\"remotes\":[", 13) != ESP_OK) return ESP_FAIL;
  for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
    sc_remote_sensor_config_t remote_config;
    uint8_t transport;
    config = sc_config_acquire();
    remote_config = config->remote_sensor[i];
    transport = config->remote_transport[i];
    sc_config_release();
    sc_remote_sensor_status_t status;
    sc_automation_remote_status(i, &status);
    char topic[SC_REMOTE_TOPIC_LEN * 6 + 1];
    char key[SC_REMOTE_KEY_LEN * 6 + 1];
    char label[SC_REMOTE_LABEL_LEN * 6 + 1];
    json_escape(topic, sizeof(topic), remote_config.topic);
    json_escape(label, sizeof(label), remote_config.label);
    if (transport == SC_REMOTE_MODBUS_TCP) {
      sc_remote_modbus_config_t m;
      sc_remote_modbus_decode(&remote_config, &m);
      n = snprintf(chunk, sizeof(chunk),
        "%s{\"id\":%u,\"transport\":1,\"host\":\"%s\",\"port\":%u,"
        "\"unit\":%u,\"function\":%u,\"address\":%u,\"valueType\":%u,"
        "\"wordOrder\":%u,\"poll\":%u,\"scale\":%.6g,\"offset\":%.6g,"
        "\"label\":\"%s\",\"valid\":%s,\"value\":%.6g,\"age\":%u}",
        i ? "," : "", (unsigned)(i + 1), topic, (unsigned)m.port,
        (unsigned)m.unit, (unsigned)m.function, (unsigned)m.address,
        (unsigned)m.value_type, (unsigned)m.word_order, (unsigned)m.poll_sec,
        (double)m.scale, (double)m.offset, label,
        status.valid ? "true" : "false", (double)status.value,
        (unsigned)status.age_sec);
    } else {
      json_escape(key, sizeof(key), remote_config.json_key);
      n = snprintf(chunk, sizeof(chunk),
        "%s{\"id\":%u,\"transport\":0,\"topic\":\"%s\",\"jsonKey\":\"%s\","
        "\"label\":\"%s\",\"valid\":%s,\"value\":%.6g,\"age\":%u}",
        i ? "," : "", (unsigned)(i + 1), topic, key, label,
        status.valid ? "true" : "false", (double)status.value,
        (unsigned)status.age_sec);
    }
    if (n < 0 || n >= (int)sizeof(chunk) ||
        httpd_resp_send_chunk(r, chunk, n) != ESP_OK) return ESP_FAIL;
  }
  if (httpd_resp_send_chunk(r, "]}", 2) != ESP_OK) return ESP_FAIL;
  return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t automation_reply(httpd_req_t *r, bool ok, const char *message)
{
  char body[192], escaped[128];
  json_escape(escaped, sizeof(escaped), message);
  snprintf(body, sizeof(body), "{\"ok\":%s,\"message\":\"%s\"}",
           ok ? "true" : "false", escaped);
  if (!ok) httpd_resp_set_status(r, HTTPD_400);
  return json_text(r, body);
}

static esp_err_t automation_rule_save(httpd_req_t *r)
{
  char body[512];
  if (recv_body(r, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
  int index = qint(body, "id", 0) - 1;
  if (index < 0 || index >= SC_MAX_AUTOMATION_RULES)
    return automation_reply(r, false, "Invalid rule slot");
  sc_config_t c;
  sc_config_snapshot(&c);
  sc_automation_rule_t rule = {0};
  sc_modbus_rule_target_t target = {0};
  int output = qint(body, "output", 0);
  int port = qint(body, "mbPort", 502);
  int unit = qint(body, "mbUnit", 1);
  int address = qint(body, "mbAddress", 0);
  char host[16] = {0};
  qstr(body, "mbHost", host, sizeof(host));
  target.remote = output == 1;
  target.send_clear = qint(body, "mbClear", 1) != 0;
  if (target.remote) {
    struct in_addr ip;
    if (!valid_modbus_host(host) || !inet_aton(host, &ip) ||
        port < 1 || port > 65535 || unit < 1 || unit > 247 ||
        address < 0 || address > 65535)
      return automation_reply(r, false, "Invalid Modbus target");
    target.ipv4 = ip.s_addr;
    target.port = port;
    target.unit = unit;
    target.address = address;
  }
  rule.enabled = qint(body, "enabled", 0) != 0;
  rule.target_relay = qint(body, "target", 0);
  rule.action = qint(body, "action", 0);
  rule.logic = qint(body, "logic", 0);
  rule.trigger_mode = qint(body, "trigger", 0);
  rule.gate_timer = qint(body, "gate", 0);
  int hold = qint(body, "hold", 0), cooldown = qint(body, "cooldown", 0);
  rule.hold_sec = hold < 0 ? 0 : hold > UINT16_MAX ? UINT16_MAX : hold;
  rule.cooldown_sec = cooldown < 0 ? 0 : cooldown > UINT16_MAX ? UINT16_MAX : cooldown;
  rule.condition[0].source = qint(body, "c0source", 0);
  rule.condition[0].op = qint(body, "c0op", 0);
  bool values_ok = qcondition_value(body, "c0value1", &rule.condition[0].value1) &&
                   qcondition_value(body, "c0value2", &rule.condition[0].value2);
  rule.condition[1].source = qint(body, "c1source", 0);
  rule.condition[1].op = qint(body, "c1op", 0);
  values_ok = qcondition_value(body, "c1value1", &rule.condition[1].value1) &&
              qcondition_value(body, "c1value2", &rule.condition[1].value2) &&
              values_ok;
  if (output < 0 || output > 1 ||
      (!target.remote && rule.target_relay >= c.relay_count) ||
      rule.action > SC_AUTOMATION_ACTION_TOGGLE ||
      rule.logic > SC_AUTOMATION_LOGIC_OR ||
      rule.trigger_mode > SC_AUTOMATION_PERIODIC ||
      rule.gate_timer > SC_MAX_TIMERS ||
      !automation_source_valid(rule.condition[0].source) ||
      !automation_source_valid(rule.condition[1].source) ||
      rule.condition[0].op > SC_AUTOMATION_EQUAL ||
      rule.condition[1].op > SC_AUTOMATION_EQUAL ||
      !values_ok ||
      (rule.enabled && rule.condition[0].source == SC_AUTOMATION_SOURCE_NONE) ||
      !isfinite(rule.condition[0].value1) || !isfinite(rule.condition[0].value2) ||
      !isfinite(rule.condition[1].value1) || !isfinite(rule.condition[1].value2))
    return automation_reply(r, false, "Invalid rule configuration");
  if (rule.trigger_mode == SC_AUTOMATION_PERIODIC &&
      rule.action == SC_AUTOMATION_ACTION_TOGGLE)
    return automation_reply(r, false, "Periodic Toggle is not allowed");
  if (target.remote && rule.action == SC_AUTOMATION_ACTION_TOGGLE)
    return automation_reply(r, false, "Remote Toggle is not supported");
  if ((rule.condition[0].op == SC_AUTOMATION_BETWEEN &&
       rule.condition[0].value2 < rule.condition[0].value1) ||
      (rule.condition[1].source != SC_AUTOMATION_SOURCE_NONE &&
       rule.condition[1].op == SC_AUTOMATION_BETWEEN &&
       rule.condition[1].value2 < rule.condition[1].value1))
    return automation_reply(r, false, "Between upper value must not be smaller");
  c.automation[index] = rule;
  c.modbus_rule[index] = target;
  if (sc_config_replace(&c) != ESP_OK)
    return automation_reply(r, false, "Could not save rule");
  sc_automation_reset_rule(index);
  sc_modbus_rule_submit(index, NULL, false);
  if (target.remote && rule.enabled && sc_modbus_ensure_started() != ESP_OK)
    return automation_reply(r, false, "Rule saved but Modbus task could not start");
  return automation_reply(r, true, "Rule saved");
}

static esp_err_t automation_remote_save(httpd_req_t *r)
{
  char body[512];
  if (recv_body(r, body, sizeof(body)) != ESP_OK) return ESP_FAIL;
  int index = qint(body, "id", 0) - 1;
  if (index < 0 || index >= SC_MAX_REMOTE_SENSORS)
    return automation_reply(r, false, "Invalid remote sensor slot");
  sc_config_t c;
  sc_config_snapshot(&c);
  sc_remote_sensor_config_t remote = {0};
  int transport = qint(body, "transport", 0);
  if (transport != SC_REMOTE_MQTT && transport != SC_REMOTE_MODBUS_TCP)
    return automation_reply(r, false, "Invalid sensor protocol");
  char topic[SC_REMOTE_TOPIC_LEN + 1] = {0};
  char json_key[SC_REMOTE_KEY_LEN + 1] = {0};
  char label[SC_REMOTE_LABEL_LEN + 1] = {0};
  qstr(body, transport == SC_REMOTE_MQTT ? "topic" : "host", topic, sizeof(topic));
  if (transport == SC_REMOTE_MQTT)
    qstr(body, "jsonKey", json_key, sizeof(json_key));
  qstr(body, "label", label, sizeof(label));
  if (strlen(topic) >= SC_REMOTE_TOPIC_LEN ||
      strlen(json_key) >= SC_REMOTE_KEY_LEN ||
      strlen(label) >= SC_REMOTE_LABEL_LEN)
    return automation_reply(r, false, "Remote sensor field is too long");
  memcpy(remote.topic, topic, strlen(topic) + 1);
  memcpy(remote.label, label, strlen(label) + 1);
  if (transport == SC_REMOTE_MQTT) {
    memcpy(remote.json_key, json_key, strlen(json_key) + 1);
    if (remote.topic[0] && !valid_publish_topic(remote.topic))
      return automation_reply(r, false, "Invalid MQTT topic");
    for (uint8_t i = 0; remote.topic[0] && i < SC_MAX_REMOTE_SENSORS; i++)
      if (i != index && c.remote_transport[i] == SC_REMOTE_MQTT &&
          !strcmp(remote.topic, c.remote_sensor[i].topic))
        return automation_reply(r, false, "MQTT topic is already assigned");
  } else {
    int port = qint(body, "port", 502);
    int unit = qint(body, "unit", 1);
    int function = qint(body, "function", 3);
    int address = qint(body, "address", 0);
    int value_type = qint(body, "valueType", SC_MODBUS_U16);
    int word_order = qint(body, "wordOrder", 0);
    int poll = qint(body, "poll", 10);
    if (port < 1 || port > 65535 || unit < 1 || unit > 247 ||
        function < 1 || function > 4 || address < 0 || address > 65535 ||
        value_type < 0 || value_type > SC_MODBUS_F32 ||
        word_order < 0 || word_order > 3 || poll < 5 ||
        poll > SC_REMOTE_STALE_SEC)
      return automation_reply(r, false, "Invalid Modbus settings");
    sc_remote_modbus_config_t m = {
      .port = port, .unit = unit, .function = function, .address = address,
      .value_type = value_type, .word_order = word_order, .poll_sec = poll,
      .scale = qfloat(body, "scale", 1.0f),
      .offset = qfloat(body, "offset", 0.0f)
    };
    if (!valid_modbus_settings(remote.topic, &m))
      return automation_reply(r, false, "Invalid Modbus host or register type");
    sc_remote_modbus_encode(&remote, &m);
  }
  char old_topic[SC_REMOTE_TOPIC_LEN];
  snprintf(old_topic, sizeof(old_topic), "%s",
           c.remote_transport[index] == SC_REMOTE_MQTT
               ? c.remote_sensor[index].topic : "");
  c.remote_sensor[index] = remote;
  c.remote_transport[index] = transport;
  if (sc_config_replace(&c) != ESP_OK)
    return automation_reply(r, false, "Could not save remote sensor");
  sc_automation_reset_remote(index);
  sc_modbus_remote_changed(index);
  sc_mqtt_update_remote_subscription(old_topic,
      transport == SC_REMOTE_MQTT ? remote.topic : "");
  if (transport == SC_REMOTE_MODBUS_TCP &&
      sc_modbus_ensure_started() != ESP_OK)
    return automation_reply(r, false, "Sensor saved; Modbus worker needs restart");
  return automation_reply(r, true, remote.topic[0] ? "Remote sensor saved" : "Remote sensor cleared");
}

static esp_err_t input_config(httpd_req_t *r) {
  sc_config_t c;
  sc_config_snapshot(&c);
  sc_input_status_t status[SC_MAX_INPUTS];
  sc_inputs_snapshot(status);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "count", c.input_count);
  cJSON_AddBoolToObject(j, "dsActive", c.ds18b20_active);
  cJSON_AddNumberToObject(j, "tankGpio", c.ds18b20_gpio);
  cJSON_AddNumberToObject(j, "panelGpio", c.ds18b20_second_gpio);
  cJSON *a = cJSON_AddArrayToObject(j, "inputs");
  for (uint8_t i = 0; i < c.input_count && i < SC_MAX_INPUTS; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "index", i);
    cJSON_AddNumberToObject(o, "gpio", c.input[i].gpio);
    cJSON_AddNumberToObject(o, "mode", c.input[i].mode);
    cJSON_AddNumberToObject(o, "relay", c.input[i].relay);
    cJSON_AddStringToObject(o, "topic", c.input[i].topic);
    cJSON_AddBoolToObject(o, "configured", status[i].configured);
    cJSON_AddStringToObject(
        o, "state",
        status[i].configured ? (status[i].level ? "ON" : "OFF") : "NA");
    cJSON_AddItemToArray(a, o);
  }
  return js(r, j);
}
static esp_err_t apply_inputs(httpd_req_t *r) {
  char body[1200];
  if (recv_body(r, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;
  sc_config_t c;
  sc_config_snapshot(&c);
  for (uint8_t i = 0; i < c.input_count && i < SC_MAX_INPUTS; i++) {
    char key[16];
    snprintf(key, sizeof(key), "mode%u", (unsigned)i);
    int mode = qint(body, key, c.input[i].mode);
    snprintf(key, sizeof(key), "relay%u", (unsigned)i);
    int relay = qint(body, key, c.input[i].relay);
    snprintf(key, sizeof(key), "topic%u", (unsigned)i);
    qstr(body, key, c.input[i].topic, sizeof(c.input[i].topic));
    if (mode < SC_INPUT_NONE || mode > SC_INPUT_TEMPERATURE)
      return send_error(r, HTTPD_400, "invalid input mode");
    if (mode == SC_INPUT_TEMPERATURE && c.input[i].gpio != c.ds18b20_gpio &&
        c.input[i].gpio != c.ds18b20_second_gpio)
      return send_error(r, HTTPD_400,
                        "temperature mode requires a configured DS18B20 GPIO");
    if ((mode == SC_INPUT_RELAY_TOGGLE || mode == SC_INPUT_COPY_TO_RELAY) &&
        (relay < 0 || relay >= c.relay_count))
      return send_error(r, HTTPD_400, "relay mapping is required");
    if (c.ds18b20_active &&
        (c.input[i].gpio == c.ds18b20_gpio ||
         c.input[i].gpio == c.ds18b20_second_gpio) &&
        mode > SC_INPUT_NONE && mode < SC_INPUT_TEMPERATURE)
      return send_error(
          r, HTTPD_400,
          "disable DS18B20 before using its GPIO as a digital input");
    c.input[i].mode = (uint8_t)mode;
    c.input[i].relay =
        (mode == SC_INPUT_RELAY_TOGGLE || mode == SC_INPUT_COPY_TO_RELAY)
            ? (int8_t)relay
            : -1;
    if (mode == SC_INPUT_TEMPERATURE)
      c.ds18b20_active = 1;
  }
  if (c.ds18b20_active) {
    for (uint8_t i = 0; i < c.input_count && i < SC_MAX_INPUTS; i++) {
      bool sensor_pin = c.input[i].gpio == c.ds18b20_gpio ||
                        c.input[i].gpio == c.ds18b20_second_gpio;
      bool digital_mode = c.input[i].mode > SC_INPUT_NONE &&
                          c.input[i].mode < SC_INPUT_TEMPERATURE;
      if (sensor_pin && digital_mode)
        return send_error(
            r, HTTPD_400,
            "DS18B20 GPIOs cannot also be configured as digital inputs");
    }
  }
  if (!sc_sensors_validate_config(&c))
    return send_error(r, HTTPD_400, "DS18B20 GPIOs are invalid or already in use");
  if (sc_config_replace(&c) != ESP_OK)
    return send_error(r, HTTPD_500, "save failed");
  if (sc_sensors_ensure_started() != ESP_OK)
    return send_error(r, HTTPD_500, "Saved; sensor task could not start, restart device");
  httpd_resp_set_type(r, "text/html");
  return httpd_resp_sendstr(
      r, "<!doctype html><meta name='viewport' "
         "content='width=device-width'><title>Inputs saved</title><body "
         "style='font:14px Arial;padding:24px'><h2>Input configuration "
         "saved</h2><p>Restart manually to apply the new GPIO configuration.</p>"
         "<p><a href='/Input_Relays_Map.html'>Back to inputs</a></p></body>");
}
static esp_err_t temperature_config(httpd_req_t *r) {
  sc_temp_control_config_t control;
  char tank_topic[SC_TOPIC_LEN], panel_topic[SC_TOPIC_LEN];
  const sc_config_t *c = sc_config_acquire();
  control = c->temp_control;
  bool ds_active = c->ds18b20_active;
  uint8_t tank_gpio = c->ds18b20_gpio, panel_gpio = c->ds18b20_second_gpio;
  memcpy(tank_topic, c->temp_topic1, sizeof(tank_topic));
  memcpy(panel_topic, c->temp_topic2, sizeof(panel_topic));
  sc_config_release();
  sc_sensor_snapshot_t s;
  sc_sensors_snapshot(&s);
  char low[32], high[32], buffer[32], tank[32] = "null", panel[32] = "null", delta[32];
  if (!sc_format_fixed(low, sizeof(low), control.low_c, 4) ||
      !sc_format_fixed(high, sizeof(high), control.high_c, 4) ||
      !sc_format_fixed(buffer, sizeof(buffer), control.buffer_c, 4) ||
      !sc_format_fixed(delta, sizeof(delta), s.temp_delta_c, 4))
    return send_error(r, HTTPD_500, "invalid temperature configuration");
  if (s.temp1_valid) sc_format_fixed(tank, sizeof(tank), s.temp1_c, 4);
  if (s.temp2_valid) sc_format_fixed(panel, sizeof(panel), s.temp2_c, 4);
  char chunk[224];
  int n = snprintf(chunk, sizeof(chunk),
    "{\"enabled\":%s,\"relay\":%u,\"low\":%s,\"high\":%s,\"buffer\":%s,"
    "\"dsActive\":%s,\"tankGpio\":%u,\"panelGpio\":%u",
    control.enabled ? "true" : "false", (unsigned)control.relay, low, high, buffer,
    ds_active ? "true" : "false", (unsigned)tank_gpio, (unsigned)panel_gpio);
  if (n < 0 || n >= (int)sizeof(chunk)) return ESP_FAIL;
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Connection", "close");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  json_output_t out = {.req = r};
  json_write(&out, chunk, (size_t)n);
  json_write_field(&out, "tankTopic", tank_topic);
  json_write_field(&out, "panelTopic", panel_topic);
  n = snprintf(chunk, sizeof(chunk),
    ",\"tank\":%s,\"panel\":%s,\"delta\":%s,\"relayState\":\"%s\"}",
    tank, panel, delta, sc_relay_get(control.relay) ? "ON" : "OFF");
  if (n < 0 || n >= (int)sizeof(chunk)) return ESP_FAIL;
  json_write(&out, chunk, (size_t)n);
  return json_finish(&out);
}
static esp_err_t apply_temperature(httpd_req_t *r) {
  char body[900];
  if (recv_body(r, body, sizeof(body)) != ESP_OK)
    return ESP_FAIL;
  sc_config_t c;
  sc_config_snapshot(&c);
  bool enabled = query_has(body, "enabled");
  int relay = qint(body, "relay", c.temp_control.relay);
  float low = qfloat(body, "low", c.temp_control.low_c),
        high = qfloat(body, "high", c.temp_control.high_c),
        buffer = qfloat(body, "buffer", c.temp_control.buffer_c);
  qstr(body, "tankTopic", c.temp_topic1, sizeof(c.temp_topic1));
  qstr(body, "panelTopic", c.temp_topic2, sizeof(c.temp_topic2));
  if (relay < 0 || relay >= c.relay_count)
    return send_error(r, HTTPD_400, "invalid relay");
  if (!isfinite(low) || !isfinite(high) || !isfinite(buffer) || low <= 0.0f ||
      high < 0.0f || (high != 0.0f && high <= low) || high >= 125.0f ||
      buffer <= 0.0f)
    return send_error(r, HTTPD_400, "invalid temperature limits");
  if (!valid_publish_topic(c.temp_topic1) ||
      !valid_publish_topic(c.temp_topic2))
    return send_error(r, HTTPD_400,
                      "temperature MQTT topics must be non-empty publish topics");
  c.temp_control.enabled = enabled;
  c.temp_control.relay = (uint8_t)relay;
  c.temp_control.low_c = low;
  c.temp_control.high_c = high;
  c.temp_control.buffer_c = buffer;
  if (enabled) {
    c.ds18b20_active = 1;
    for (uint8_t i = 0; i < c.input_count && i < SC_MAX_INPUTS; i++)
      if (c.input[i].gpio == c.ds18b20_gpio ||
          c.input[i].gpio == c.ds18b20_second_gpio) {
        c.input[i].mode = SC_INPUT_TEMPERATURE;
        c.input[i].relay = -1;
      }
  }
  if (!sc_sensors_validate_config(&c))
    return send_error(r, HTTPD_400, "DS18B20 GPIOs are invalid or already in use");
  if (sc_config_replace(&c) != ESP_OK)
    return send_error(r, HTTPD_500, "save failed");
  if (sc_sensors_ensure_started() != ESP_OK)
    return send_error(r, HTTPD_500, "Saved; sensor task could not start, restart device");
  httpd_resp_set_type(r, "text/html");
  return httpd_resp_sendstr(
      r, "<!doctype html><meta name='viewport' "
         "content='width=device-width'><title>Temperature saved</title><body "
         "style='font:14px Arial;padding:24px'><h2>Temperature control "
         "saved</h2><p>Temperature settings are active.</p><p><a href='/AdvancedTemperatureConf.html'>Back to "
         "temperature control</a></p></body>");
}

static esp_err_t apply(httpd_req_t *r)
{
  char q[CONFIG_HTTPD_MAX_URI_LEN + 1];
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK)
    return send_error(r, HTTPD_400, "missing query");
  sc_config_t c;
  sc_config_snapshot(&c);
  qstr(q, "PhyLoc", c.phy_loc, sizeof(c.phy_loc));
  char submitted_ssid[SC_SSID_LEN] = {0};
  qstr(q, "ssid", submitted_ssid, sizeof(submitted_ssid));
  if (submitted_ssid[0]) snprintf(c.ssid, sizeof(c.ssid), "%s", submitted_ssid);
  char pw[SC_PASS_LEN] = {0};
  if (httpd_query_key_value(q, "pass", pw, sizeof(pw)) == ESP_OK && pw[0])
    snprintf(c.wifi_password, sizeof(c.wifi_password), "%s", pw);
  qstr(q, "timeserver", c.timeserver, sizeof(c.timeserver));
  qstr(q, "Web_User", c.web_user, sizeof(c.web_user));
  if (!c.web_user[0] || strchr(c.web_user, ':'))
    return send_error(r, HTTPD_400, "invalid web username");
  char web_password[SC_PASS_LEN] = {0};
  qstr(q, "Web_Password", web_password, sizeof(web_password));
  if (web_password[0]) snprintf(c.web_password, sizeof(c.web_password), "%s", web_password);
  c.timezone_hours = qint(q, "ntptz", c.timezone_hours);
  c.mqtt_active = httpd_query_key_value(q, "MQTT_Active", pw, sizeof(pw)) == ESP_OK;
  c.ha_discovery_active = httpd_query_key_value(q, "HA_Discovery_Active", pw, sizeof(pw)) == ESP_OK;
  qstr(q, "MQTT_BROKER", c.mqtt_broker, sizeof(c.mqtt_broker));
  c.mqtt_port = qint(q, "MQTT_B_PRT", c.mqtt_port);
  c.mqtt_keepalive_sec = qint(q, "MQTT_KeepAliveSeconds", c.mqtt_keepalive_sec);
  qstr(q, "mqttUser", c.mqtt_user, sizeof(c.mqtt_user));
  memset(pw, 0, sizeof(pw));
  if (httpd_query_key_value(q, "mqttPass", pw, sizeof(pw)) == ESP_OK && pw[0])
    snprintf(c.mqtt_password, sizeof(c.mqtt_password), "%s", pw);
  c.modbus_active = httpd_query_key_value(q, "Modbus_Active", pw, sizeof(pw)) == ESP_OK;
  c.modbus_port = qint(q, "Modbus_Port", c.modbus_port);
  int unit_id = qint(q, "Modbus_Slave_ID", c.modbus_unit_id);
  if (unit_id < 1 || unit_id > 247)
    return send_error(r, HTTPD_400, "invalid Modbus slave ID");
  c.modbus_unit_id = (uint8_t)unit_id;
  c.ds18b20_active = httpd_query_key_value(q, "DS18B20_Active", pw, sizeof(pw)) == ESP_OK;
  int first_gpio = qint(q, "TempSensorPin", c.ds18b20_gpio);
  int second_gpio = qint(q, "SecondTempSensorPin", c.ds18b20_second_gpio);
  if (first_gpio < 0 || first_gpio > 255 || second_gpio < 0 || second_gpio > 255)
    return send_error(r, HTTPD_400, "invalid DS18B20 GPIO");
  c.ds18b20_gpio = (uint8_t)first_gpio;
  c.ds18b20_second_gpio = (uint8_t)second_gpio;
  if (!sc_sensors_validate_config(&c))
    return send_error(r, HTTPD_400, "DS18B20 GPIOs are invalid or already in use");
  if (sc_config_replace(&c) != ESP_OK)
    return send_error(r, HTTPD_500, "save failed");
  if (sc_sensors_ensure_started() != ESP_OK)
    return send_error(r, HTTPD_500, "Saved; sensor task could not start, restart device");
  httpd_resp_set_type(r, "text/html");
  return httpd_resp_sendstr(r, "<h2>Configuration saved</h2><p>DS18B20 settings are active. "
      "Restart manually from the configuration page to apply network and service changes.</p>"
      "<p><a href='/config.html'>Back to configuration</a></p>");
}
static esp_err_t applyrelay(httpd_req_t*r){char q[CONFIG_HTTPD_MAX_URI_LEN + 1];if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return ESP_FAIL;sc_config_t c;sc_config_snapshot(&c);int n=qint(q,"RELAYNB",0);if(n<0||n>=c.relay_count)return ESP_FAIL;char old_sub[SC_TOPIC_LEN],old_pub[SC_TOPIC_LEN];snprintf(old_sub,sizeof(old_sub),"%s",c.relay[n].sub_topic);snprintf(old_pub,sizeof(old_pub),"%s",c.relay[n].pub_topic);qstr(q,"PUB_TOPIC1",c.relay[n].pub_topic,sizeof(c.relay[n].pub_topic));qstr(q,"STATE_PUB_TOPIC",c.relay[n].state_pub_topic,sizeof(c.relay[n].state_pub_topic));qstr(q,"SUB_TOPIC1",c.relay[n].sub_topic,sizeof(c.relay[n].sub_topic));qstr(q,"LWILL_TOPIC",c.relay[n].lwill_topic,sizeof(c.relay[n].lwill_topic));qstr(q,"ACS_AMPS",c.relay[n].acs_topic,sizeof(c.relay[n].acs_topic));c.relay[n].ttl_sec=qu32(q,"ttl",c.relay[n].ttl_sec);c.relay[n].tta_sec=qu32(q,"tta",c.relay[n].tta_sec);char acb[8];c.relay[n].acs_enabled=httpd_query_key_value(q,"ACS_Active",acb,sizeof(acb))==ESP_OK;c.relay[n].acs_model_amps=qint(q,"ACS_Sensor_Model",c.relay[n].acs_model_amps);c.relay[n].max_current=qfloat(q,"Max_Current",c.relay[n].max_current);if(sc_config_replace(&c)!=ESP_OK)return ESP_FAIL;sc_relay_set_timing((uint8_t)n,true,c.relay[n].ttl_sec);sc_relay_set_timing((uint8_t)n,false,c.relay[n].tta_sec);sc_mqtt_relay_config_saved((uint8_t)n,old_sub,old_pub);httpd_resp_set_type(r,"text/html");return httpd_resp_sendstr(r,"<h2>Relay saved</h2><p>TTL and TTA are active and synchronized to retained MQTT topics. Restart manually to apply GPIO changes.</p><p><a href='/RelayConfig.html'>Back to relays</a></p>");}

static int64_t civil_days(int y,unsigned m,unsigned d){y-=m<=2;const int era=(y>=0?y:y-399)/400;const unsigned yoe=(unsigned)(y-era*400);const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;const unsigned doe=yoe*365+yoe/4-yoe/100+doy;return (int64_t)era*146097+(int64_t)doe-719468;}
static bool parse_date(const char*s,int*y,int*m,int*d){return s&&sscanf(s,"%d-%d-%d",y,m,d)==3&&*m>=1&&*m<=12&&*d>=1&&*d<=31;}
static bool parse_hhmm(const char*s,int*h,int*m){return s&&sscanf(s,"%d:%d",h,m)==2&&*h>=0&&*h<=23&&*m>=0&&*m<=59;}
static int64_t local_epoch(const char*date,int hour,int minute){int y,m,d;if(!parse_date(date,&y,&m,&d))return -1;return civil_days(y,(unsigned)m,(unsigned)d)*86400+(int64_t)hour*3600+minute*60;}
static int date_number(const char*s){int y,m,d;return parse_date(s,&y,&m,&d)?y*10000+m*100+d:0;}
static bool query_has(const char*q,const char*key){char value[16];return httpd_query_key_value(q,key,value,sizeof(value))==ESP_OK;}

static int32_t timer_next_seconds(const sc_timer_config_t*t,const sc_timer_schedule_t*s,time_t now){if(!t->enabled||t->type==SC_TIMER_DISABLED)return -1;if(t->type==SC_TIMER_ONCE)return t->once_epoch>(uint32_t)now?(int32_t)(t->once_epoch-now):-1;struct tm tm;gmtime_r(&now,&tm);int today=(tm.tm_year+1900)*10000+(tm.tm_mon+1)*100+tm.tm_mday;int first=date_number(s->date_from),last=date_number(s->date_to);if(last&&today>last)return -1;if(first&&today<first){int64_t start=local_epoch(s->date_from,t->start_hour,t->start_minute);return start>now&&start-now<=INT32_MAX?(int32_t)(start-now):-1;}int now_sec=tm.tm_hour*3600+tm.tm_min*60+tm.tm_sec;int start=t->start_hour*3600+t->start_minute*60;if(t->type==SC_TIMER_DAILY){int delta=start-now_sec;return delta>0?delta:delta+86400;}if(t->type==SC_TIMER_WEEKLY){int today_wd=(tm.tm_wday+6)%7;for(int offset=0;offset<=7;offset++){int day=(today_wd+offset)%7;if(t->weekdays&(1u<<day)){int delta=offset*86400+start-now_sec;if(delta>0)return delta;}}return -1;}if(t->type==SC_TIMER_MONTHLY){int target=t->month_day?t->month_day:1;int mdays=31;if(tm.tm_mon==3||tm.tm_mon==5||tm.tm_mon==8||tm.tm_mon==10)mdays=30;else if(tm.tm_mon==1){int year=tm.tm_year+1900;mdays=((year%4==0&&year%100!=0)||year%400==0)?29:28;}if(tm.tm_mday<target||(tm.tm_mday==target&&start>now_sec))return (target-tm.tm_mday)*86400+start-now_sec;return (mdays-tm.tm_mday+target)*86400+start-now_sec;}return -1;}

typedef struct{const char*token;const char*value;} timer_replacement_t;
typedef struct{httpd_req_t*req;char data[512];size_t used;esp_err_t error;} timer_output_t;
static void timer_output_flush(timer_output_t*out){if(out->error==ESP_OK&&out->used){out->error=httpd_resp_send_chunk(out->req,out->data,out->used);out->used=0;}}
static void timer_output_write(timer_output_t*out,const char*s,size_t n){while(n&&out->error==ESP_OK){size_t room=sizeof(out->data)-out->used;size_t take=n<room?n:room;memcpy(out->data+out->used,s,take);out->used+=take;s+=take;n-=take;if(out->used==sizeof(out->data))timer_output_flush(out);}}
static esp_err_t timer_render(httpd_req_t*r,const timer_replacement_t*values,size_t count){timer_output_t out={.req=r,.error=ESP_OK};const char*p=sc_timer_html;while(*p&&out.error==ESP_OK){bool matched=false;for(size_t i=0;i<count;i++){size_t n=strlen(values[i].token);if(!strncmp(p,values[i].token,n)){timer_output_write(&out,values[i].value,strlen(values[i].value));p+=n;matched=true;break;}}if(!matched){timer_output_write(&out,p,1);p++;}}timer_output_flush(&out);if(out.error==ESP_OK)out.error=httpd_resp_send_chunk(r,NULL,0);return out.error;}
static esp_err_t timer_page(httpd_req_t*r){char q[96],v[12];int selected=0;if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK&&httpd_query_key_value(q,"GetTimer",v,sizeof(v))==ESP_OK)selected=atoi(v);if(selected<1||selected>SC_MAX_TIMERS)selected=0;sc_timer_config_t timer={0};sc_timer_schedule_t schedule={.date_from="1970-01-01",.date_to="2100-01-01"};const sc_config_t*cfg=sc_config_acquire();uint8_t relay_count=cfg->relay_count;if(selected){timer=cfg->timer[selected-1];schedule=cfg->timer_schedule[selected-1];}sc_config_release();const sc_timer_config_t*t=&timer;const sc_timer_schedule_t*s=&schedule;sc_timer_status_t status={0};if(selected)sc_timer_status(selected,&status);char system_time[40],uptime[32],number[8],type[8],from[8],to[8],hours[8],minutes[8],month_day[8],relay_options[384];sc_time_format(system_time,sizeof(system_time));uptime_string(uptime,sizeof(uptime));snprintf(number,sizeof(number),"%d",selected);snprintf(type,sizeof(type),"%u",(unsigned)(t->type?t->type:SC_TIMER_ONCE));snprintf(from,sizeof(from),"%02u:%02u",(unsigned)t->start_hour,(unsigned)t->start_minute);snprintf(to,sizeof(to),"%02u:%02u",(unsigned)s->end_hour,(unsigned)s->end_minute);snprintf(hours,sizeof(hours),"%u",(unsigned)s->duration_hours);snprintf(minutes,sizeof(minutes),"%u",(unsigned)s->duration_minutes);snprintf(month_day,sizeof(month_day),"%u",(unsigned)(t->month_day?t->month_day:1));size_t off=0;off+=(size_t)snprintf(relay_options+off,sizeof(relay_options)-off,"<option value=\"255\"%s>(none &mdash; schedule window only)</option>",t->relay==UINT8_MAX?" selected":"");for(uint8_t i=0;i<relay_count&&off<sizeof(relay_options);i++)off+=(size_t)snprintf(relay_options+off,sizeof(relay_options)-off,"<option value=\"%u\"%s>Relay %u</option>",(unsigned)i,t->relay==i?" selected":"",(unsigned)i);const char*checked="1\" checked=\"";const char*unchecked="";const char*tstate=selected&&t->relay<relay_count?(status.paused?"PAUSED":"NOT PAUSED"):"NA";const char*rstate=selected&&t->relay<relay_count?(sc_relay_get(t->relay)?"ON":"OFF"):"NA";timer_replacement_t values[]={{"%systemtime%",system_time},{"%uptime%",uptime},{"%TNBT%",number},{"%TMTYPEedit%",type},{"%Dfrom%",s->date_from},{"%DTo%",s->date_to},{"%TFrom%",from},{"%TTo%",to},{"%Mark_Hours%",hours},{"%Mark_Minutes%",minutes},{"%MONTHDAY%",month_day},{"%TIMER_RELAY_OPTIONS%",relay_options},{"%CEnabled%",t->enabled?checked:unchecked},{"%CMonday%",(t->weekdays&1)?checked:unchecked},{"%CTuesday%",(t->weekdays&2)?checked:unchecked},{"%CWednesday%",(t->weekdays&4)?checked:unchecked},{"%CThursday%",(t->weekdays&8)?checked:unchecked},{"%CFriday%",(t->weekdays&16)?checked:unchecked},{"%CSaturday%",(t->weekdays&32)?checked:unchecked},{"%CSunday%",(t->weekdays&64)?checked:unchecked},{"%TSTATE%",tstate},{"%RSTATE%",rstate}};httpd_resp_set_type(r,"text/html");httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"Connection","close");return timer_render(r,values,sizeof(values)/sizeof(values[0]));}

static esp_err_t timer_status(httpd_req_t*r){static const char*type_name[]={"Unknown","Date Range","Daily","Weekly","Monthly"};sc_timer_config_t timers[SC_MAX_TIMERS];sc_timer_schedule_t schedules[SC_MAX_TIMERS];const sc_config_t*cfg=sc_config_acquire();memcpy(timers,cfg->timer,sizeof(timers));memcpy(schedules,cfg->timer_schedule,sizeof(schedules));uint8_t relay_count=cfg->relay_count;sc_config_release();time_t now=sc_time_now_local();struct tm now_tm;gmtime_r(&now,&now_tm);char head[40];int hn=snprintf(head,sizeof(head),"{\"currentTime\":\"%02d:%02d:%02d\",\"timers\":[",now_tm.tm_hour,now_tm.tm_min,now_tm.tm_sec);httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");if(hn<0||hn>=(int)sizeof(head)||httpd_resp_send_chunk(r,head,hn)!=ESP_OK)return ESP_FAIL;for(int i=1;i<=SC_MAX_TIMERS;i++){sc_timer_status_t st;sc_timer_status(i,&st);const sc_timer_config_t*t=&timers[i-1];const sc_timer_schedule_t*s=&schedules[i-1];unsigned end_hour=s->end_hour,end_minute=s->end_minute;if(s->duration_hours||s->duration_minutes){uint32_t end=(t->start_hour*60+t->start_minute+s->duration_hours*60+s->duration_minutes)%(24*60);end_hour=end/60;end_minute=end%60;}int32_t seconds_to_on=st.active?-1:timer_next_seconds(t,s,now);const char*relay_state=t->relay<relay_count?(sc_relay_get(t->relay)?"ON":"OFF"):"NA";const char*name=t->type<=SC_TIMER_MONTHLY?type_name[t->type]:type_name[0];char row[768];int n=snprintf(row,sizeof(row),"%s{\"id\":%d,\"exists\":%s,\"enabled\":%s,\"timerActive\":%s,\"timerPaused\":%s,\"relayState\":\"%s\",\"relay\":%u,\"type\":%u,\"typeName\":\"%s\",\"dateFrom\":\"%s\",\"dateTo\":\"%s\",\"timeFrom\":\"%02u:%02u\",\"timeTo\":\"%02u:%02u\",\"markHours\":%u,\"markMinutes\":%u,\"monthDay\":%u,\"days\":{\"Mo\":%s,\"Tu\":%s,\"We\":%s,\"Th\":%s,\"Fr\":%s,\"Sa\":%s,\"Su\":%s},\"hour\":%u,\"minute\":%u,\"duration\":%u,\"weekdays\":%u,\"monthday\":%u,\"epoch\":%u,\"secondsToOff\":%d,\"secondsToOn\":%d}",i>1?",":"",i,st.exists?"true":"false",t->enabled?"true":"false",st.active?"true":"false",st.paused?"true":"false",relay_state,(unsigned)t->relay,(unsigned)t->type,name,s->date_from,s->date_to,(unsigned)t->start_hour,(unsigned)t->start_minute,end_hour,end_minute,(unsigned)s->duration_hours,(unsigned)s->duration_minutes,(unsigned)t->month_day,(t->weekdays&1)?"true":"false",(t->weekdays&2)?"true":"false",(t->weekdays&4)?"true":"false",(t->weekdays&8)?"true":"false",(t->weekdays&16)?"true":"false",(t->weekdays&32)?"true":"false",(t->weekdays&64)?"true":"false",(unsigned)t->start_hour,(unsigned)t->start_minute,(unsigned)t->duration_sec,(unsigned)t->weekdays,(unsigned)t->month_day,(unsigned)t->once_epoch,(int)st.seconds_to_off,(int)seconds_to_on);if(n<0||n>=(int)sizeof(row)||httpd_resp_send_chunk(r,row,n)!=ESP_OK)return ESP_FAIL;}if(httpd_resp_send_chunk(r,"]}",2)!=ESP_OK)return ESP_FAIL;return httpd_resp_send_chunk(r,NULL,0);}
static esp_err_t timer_control(httpd_req_t*r){char q[96],v[16];int relay=0;if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return send_error(r,HTTPD_400,"missing query");if(httpd_query_key_value(q,"relay",v,sizeof(v))==ESP_OK)relay=atoi(v);if(relay<0||relay>=sc_relay_count())return send_error(r,HTTPD_400,"invalid relay");if(httpd_query_key_value(q,"action",v,sizeof(v))!=ESP_OK)return send_error(r,HTTPD_400,"missing action");bool pause=!strcasecmp(v,"pause");if(!pause&&strcasecmp(v,"resume"))return send_error(r,HTTPD_400,"invalid action");bool paused=sc_timer_set_paused((uint8_t)relay,pause);cJSON*j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",true);cJSON_AddNumberToObject(j,"relay",relay);cJSON_AddBoolToObject(j,"paused",paused);return js(r,j);}
static esp_err_t timer_save(httpd_req_t*r){char q[512];if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return ESP_FAIL;int id=qint(q,"id",1)-1;if(id<0||id>=SC_MAX_TIMERS)return ESP_FAIL;sc_config_t c;sc_config_snapshot(&c);sc_timer_config_t*t=&c.timer[id];t->enabled=qint(q,"enabled",0);t->type=qint(q,"type",SC_TIMER_DAILY);t->relay=qint(q,"relay",0);t->start_hour=qint(q,"hour",0);t->start_minute=qint(q,"minute",0);t->duration_sec=qint(q,"duration",60);t->weekdays=qint(q,"weekdays",0x7f);t->month_day=qint(q,"monthday",1);t->once_epoch=(uint32_t)qint(q,"epoch",0);sc_timer_schedule_t*s=&c.timer_schedule[id];s->duration_hours=t->duration_sec/3600;s->duration_minutes=(t->duration_sec%3600)/60;uint32_t end=t->start_hour*60+t->start_minute+t->duration_sec/60;s->end_hour=(end/60)%24;s->end_minute=end%60;if(sc_config_replace(&c)!=ESP_OK)return ESP_FAIL;return httpd_resp_sendstr(r,"OK");}
static esp_err_t timer_save_form(httpd_req_t*r){char q[CONFIG_HTTPD_MAX_URI_LEN + 1];if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return send_error(r,HTTPD_400,"missing timer data");int index=qint(q,"TNumber",qint(q,"TNBT",0))-1;if(index<0||index>=SC_MAX_TIMERS)return send_error(r,HTTPD_400,"invalid timer");sc_config_t cfg;sc_config_snapshot(&cfg);sc_timer_config_t*t=&cfg.timer[index];sc_timer_schedule_t*s=&cfg.timer_schedule[index];t->enabled=query_has(q,"CEnabled");t->type=qint(q,"TMTYPEedit",SC_TIMER_ONCE);if(t->type<SC_TIMER_ONCE||t->type>SC_TIMER_MONTHLY)t->type=SC_TIMER_ONCE;t->relay=qint(q,"TRelay",0);if(t->relay>=cfg.relay_count&&t->relay!=UINT8_MAX)t->relay=0;t->weekdays=(query_has(q,"CMonday")?1:0)|(query_has(q,"CTuesday")?2:0)|(query_has(q,"CWednesday")?4:0)|(query_has(q,"CThursday")?8:0)|(query_has(q,"CFriday")?16:0)|(query_has(q,"CSaturday")?32:0)|(query_has(q,"CSunday")?64:0);t->month_day=qint(q,"MonthDay",1);if(t->month_day<1||t->month_day>31)t->month_day=1;qstr(q,"Dfrom",s->date_from,sizeof(s->date_from));qstr(q,"DTo",s->date_to,sizeof(s->date_to));char value[24]={0};int start_h=0,start_m=0,end_h=0,end_m=0;qstr(q,"TFrom",value,sizeof(value));if(!parse_hhmm(value,&start_h,&start_m))return send_error(r,HTTPD_400,"invalid start time");value[0]=0;qstr(q,"TTo",value,sizeof(value));if(!parse_hhmm(value,&end_h,&end_m))return send_error(r,HTTPD_400,"invalid end time");if(date_number(s->date_from)&&date_number(s->date_to)&&date_number(s->date_to)<date_number(s->date_from))return send_error(r,HTTPD_400,"invalid date range");t->start_hour=start_h;t->start_minute=start_m;s->end_hour=end_h;s->end_minute=end_m;int duration_hours=qint(q,"Mark_Hours",0),duration_minutes=qint(q,"Mark_Minutes",0);if(duration_hours<0)duration_hours=0;if(duration_hours>UINT16_MAX)duration_hours=UINT16_MAX;if(duration_minutes<0)duration_minutes=0;if(duration_minutes>59)duration_minutes=59;s->duration_hours=(uint16_t)duration_hours;s->duration_minutes=(uint8_t)duration_minutes;uint64_t duration=(uint64_t)s->duration_hours*3600+s->duration_minutes*60;if(!duration){if(t->type==SC_TIMER_ONCE){int64_t begin=local_epoch(s->date_from,start_h,start_m),end=local_epoch(s->date_to,end_h,end_m);if(begin<0||end<=begin)return send_error(r,HTTPD_400,"invalid timer span");duration=(uint64_t)(end-begin);}else{int begin=start_h*3600+start_m*60,end=end_h*3600+end_m*60;if(end<=begin)return send_error(r,HTTPD_400,"end time must be after start time");duration=(uint32_t)(end-begin);}}if(duration>UINT32_MAX)duration=UINT32_MAX;t->duration_sec=(uint32_t)duration;int64_t start_epoch=local_epoch(s->date_from,start_h,start_m);t->once_epoch=t->type==SC_TIMER_ONCE&&start_epoch>0?(uint32_t)start_epoch:0;if(sc_config_replace(&cfg)!=ESP_OK)return send_error(r,HTTPD_500,"save failed");httpd_resp_set_type(r,"text/html");httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"Connection","close");return httpd_resp_sendstr(r,sc_timer_saved_html);}

static cJSON *config_json(bool secrets) {
  sc_config_t c;
  sc_config_snapshot(&c);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "_version", 7);
  cJSON_AddStringToObject(j, "PhyLoc", c.phy_loc);
  cJSON_AddStringToObject(j, "ssid", c.ssid);
  if (secrets)
    cJSON_AddStringToObject(j, "pass", c.wifi_password);
  cJSON_AddStringToObject(j, "timeserver", c.timeserver);
  cJSON_AddStringToObject(j, "Web_User", c.web_user);
  if (secrets)
    cJSON_AddStringToObject(j, "Web_Password", c.web_password);
  cJSON_AddNumberToObject(j, "ntptz", c.timezone_hours);
  cJSON_AddNumberToObject(j, "MQTT_Active", c.mqtt_active);
  cJSON_AddStringToObject(j, "MQTT_BROKER", c.mqtt_broker);
  cJSON_AddNumberToObject(j, "MQTT_B_PRT", c.mqtt_port);
  cJSON_AddStringToObject(j, "mqttUser", c.mqtt_user);
  if (secrets)
    cJSON_AddStringToObject(j, "mqttPass", c.mqtt_password);
  cJSON_AddNumberToObject(j, "HA_Discovery_Active", c.ha_discovery_active);
  cJSON_AddNumberToObject(j, "Modbus_Active", c.modbus_active);
  cJSON_AddNumberToObject(j, "Modbus_Port", c.modbus_port);
  cJSON_AddNumberToObject(j, "Modbus_Slave_ID", c.modbus_unit_id);
  cJSON_AddNumberToObject(j, "DS18B20_Active", c.ds18b20_active);
  cJSON_AddNumberToObject(j, "TempSensorPin", c.ds18b20_gpio);
  cJSON_AddNumberToObject(j, "SecondTempSensorPin", c.ds18b20_second_gpio);
  cJSON_AddStringToObject(j, "TempTopic1", c.temp_topic1);
  cJSON_AddStringToObject(j, "TempTopic2", c.temp_topic2);
  cJSON *ra = cJSON_AddArrayToObject(j, "relays");
  for (int i = 0; i < c.relay_count; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "pub", c.relay[i].pub_topic);
    cJSON_AddStringToObject(o, "state", c.relay[i].state_pub_topic);
    cJSON_AddStringToObject(o, "sub", c.relay[i].sub_topic);
    cJSON_AddStringToObject(o, "acs", c.relay[i].acs_topic);
    cJSON_AddNumberToObject(o, "ttl", c.relay[i].ttl_sec);
    cJSON_AddNumberToObject(o, "tta", c.relay[i].tta_sec);
    cJSON_AddItemToArray(ra, o);
  }
  cJSON *ta = cJSON_AddArrayToObject(j, "timers");
  for (int i = 0; i < SC_MAX_TIMERS; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c.timer[i].enabled);
    cJSON_AddNumberToObject(o, "type", c.timer[i].type);
    cJSON_AddNumberToObject(o, "relay", c.timer[i].relay);
    cJSON_AddNumberToObject(o, "weekdays", c.timer[i].weekdays);
    cJSON_AddNumberToObject(o, "month_day", c.timer[i].month_day);
    cJSON_AddNumberToObject(o, "hour", c.timer[i].start_hour);
    cJSON_AddNumberToObject(o, "minute", c.timer[i].start_minute);
    cJSON_AddNumberToObject(o, "duration", c.timer[i].duration_sec);
    cJSON_AddNumberToObject(o, "epoch", c.timer[i].once_epoch);
    cJSON_AddStringToObject(o, "date_from", c.timer_schedule[i].date_from);
    cJSON_AddStringToObject(o, "date_to", c.timer_schedule[i].date_to);
    cJSON_AddNumberToObject(o, "end_hour", c.timer_schedule[i].end_hour);
    cJSON_AddNumberToObject(o, "end_minute", c.timer_schedule[i].end_minute);
    cJSON_AddNumberToObject(o, "duration_hours",
                            c.timer_schedule[i].duration_hours);
    cJSON_AddNumberToObject(o, "duration_minutes",
                            c.timer_schedule[i].duration_minutes);
    cJSON_AddItemToArray(ta, o);
  }
  cJSON *ia = cJSON_AddArrayToObject(j, "inputs");
  for (int i = 0; i < c.input_count && i < SC_MAX_INPUTS; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "gpio", c.input[i].gpio);
    cJSON_AddNumberToObject(o, "mode", c.input[i].mode);
    cJSON_AddNumberToObject(o, "relay", c.input[i].relay);
    cJSON_AddStringToObject(o, "topic", c.input[i].topic);
    cJSON_AddItemToArray(ia, o);
  }
  cJSON *tc = cJSON_AddObjectToObject(j, "temperatureControl");
  cJSON_AddBoolToObject(tc, "enabled", c.temp_control.enabled);
  cJSON_AddNumberToObject(tc, "relay", c.temp_control.relay);
  cJSON_AddNumberToObject(tc, "low", c.temp_control.low_c);
  cJSON_AddNumberToObject(tc, "high", c.temp_control.high_c);
  cJSON_AddNumberToObject(tc, "buffer", c.temp_control.buffer_c);
  cJSON *aa = cJSON_AddArrayToObject(j, "automation");
  for (int i = 0; i < SC_MAX_AUTOMATION_RULES; i++) {
    const sc_automation_rule_t *rule = &c.automation[i];
    const sc_modbus_rule_target_t *target = &c.modbus_rule[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", rule->enabled);
    cJSON_AddNumberToObject(o, "target", rule->target_relay);
    cJSON_AddNumberToObject(o, "action", rule->action);
    cJSON_AddNumberToObject(o, "logic", rule->logic);
    cJSON_AddNumberToObject(o, "trigger", rule->trigger_mode);
    cJSON_AddNumberToObject(o, "gate", rule->gate_timer);
    cJSON_AddNumberToObject(o, "hold", rule->hold_sec);
    cJSON_AddNumberToObject(o, "cooldown", rule->cooldown_sec);
    cJSON_AddNumberToObject(o, "output", target->remote);
    if (target->remote) {
      struct in_addr address = {.s_addr = target->ipv4};
      cJSON_AddStringToObject(o, "mbHost", inet_ntoa(address));
      cJSON_AddNumberToObject(o, "mbPort", target->port);
      cJSON_AddNumberToObject(o, "mbUnit", target->unit);
      cJSON_AddNumberToObject(o, "mbAddress", target->address);
      cJSON_AddBoolToObject(o, "mbClear", target->send_clear);
    }
    cJSON_AddNumberToObject(o, "c0source", rule->condition[0].source);
    cJSON_AddNumberToObject(o, "c0op", rule->condition[0].op);
    cJSON_AddNumberToObject(o, "c0v1", rule->condition[0].value1);
    cJSON_AddNumberToObject(o, "c0v2", rule->condition[0].value2);
    cJSON_AddNumberToObject(o, "c1source", rule->condition[1].source);
    cJSON_AddNumberToObject(o, "c1op", rule->condition[1].op);
    cJSON_AddNumberToObject(o, "c1v1", rule->condition[1].value1);
    cJSON_AddNumberToObject(o, "c1v2", rule->condition[1].value2);
    cJSON_AddItemToArray(aa, o);
  }
  cJSON *rsa = cJSON_AddArrayToObject(j, "remoteSensors");
  for (int i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", c.remote_sensor[i].label);
    cJSON_AddNumberToObject(o, "transport", c.remote_transport[i]);
    if (c.remote_transport[i] == SC_REMOTE_MODBUS_TCP) {
      sc_remote_modbus_config_t m;
      sc_remote_modbus_decode(&c.remote_sensor[i], &m);
      cJSON_AddStringToObject(o, "host", c.remote_sensor[i].topic);
      cJSON_AddNumberToObject(o, "port", m.port);
      cJSON_AddNumberToObject(o, "unit", m.unit);
      cJSON_AddNumberToObject(o, "function", m.function);
      cJSON_AddNumberToObject(o, "address", m.address);
      cJSON_AddNumberToObject(o, "valueType", m.value_type);
      cJSON_AddNumberToObject(o, "wordOrder", m.word_order);
      cJSON_AddNumberToObject(o, "poll", m.poll_sec);
      cJSON_AddNumberToObject(o, "scale", m.scale);
      cJSON_AddNumberToObject(o, "offset", m.offset);
    } else {
      cJSON_AddStringToObject(o, "topic", c.remote_sensor[i].topic);
      cJSON_AddStringToObject(o, "jsonKey", c.remote_sensor[i].json_key);
    }
    cJSON_AddItemToArray(rsa, o);
  }
  return j;
}
static int jtruth(cJSON *x) {
  return x && (cJSON_IsTrue(x) || (cJSON_IsNumber(x) && x->valueint != 0));
}
static void jcpy(cJSON *j, const char *k, char *dst, size_t n) {
  cJSON *x = cJSON_GetObjectItem(j, k);
  if (cJSON_IsString(x))
    snprintf(dst, n, "%s", x->valuestring);
}
static esp_err_t restore_config_object(cJSON *j) {
  sc_config_t c;
  sc_config_snapshot(&c);
  cJSON *x;
  jcpy(j, "PhyLoc", c.phy_loc, sizeof(c.phy_loc));
  jcpy(j, "ssid", c.ssid, sizeof(c.ssid));
  jcpy(j, "pass", c.wifi_password, sizeof(c.wifi_password));
  jcpy(j, "timeserver", c.timeserver, sizeof(c.timeserver));
  jcpy(j, "Web_User", c.web_user, sizeof(c.web_user));
  jcpy(j, "Web_Password", c.web_password, sizeof(c.web_password));
  jcpy(j, "MQTT_BROKER", c.mqtt_broker, sizeof(c.mqtt_broker));
  jcpy(j, "mqttUser", c.mqtt_user, sizeof(c.mqtt_user));
  jcpy(j, "mqttPass", c.mqtt_password, sizeof(c.mqtt_password));
  x = cJSON_GetObjectItem(j, "ntptz");
  if (cJSON_IsNumber(x))
    c.timezone_hours = x->valueint;
  x = cJSON_GetObjectItem(j, "MQTT_Active");
  if (x)
    c.mqtt_active = jtruth(x);
  x = cJSON_GetObjectItem(j, "MQTT_B_PRT");
  if (cJSON_IsNumber(x))
    c.mqtt_port = x->valueint;
  x = cJSON_GetObjectItem(j, "HA_Discovery_Active");
  if (x)
    c.ha_discovery_active = jtruth(x);
  x = cJSON_GetObjectItem(j, "Modbus_Active");
  if (x)
    c.modbus_active = jtruth(x);
  x = cJSON_GetObjectItem(j, "Modbus_Port");
  if (cJSON_IsNumber(x))
    c.modbus_port = x->valueint;
  x = cJSON_GetObjectItem(j, "Modbus_Slave_ID");
  if (cJSON_IsNumber(x) && x->valueint >= 1 && x->valueint <= 247)
    c.modbus_unit_id = x->valueint;
  x = cJSON_GetObjectItem(j, "DS18B20_Active");
  if (x)
    c.ds18b20_active = jtruth(x);
  x = cJSON_GetObjectItem(j, "TempSensorPin");
  if (cJSON_IsNumber(x))
    c.ds18b20_gpio = x->valueint;
  x = cJSON_GetObjectItem(j, "SecondTempSensorPin");
  if (cJSON_IsNumber(x))
    c.ds18b20_second_gpio = x->valueint;
  jcpy(j, "TempTopic1", c.temp_topic1, sizeof(c.temp_topic1));
  jcpy(j, "TempTopic2", c.temp_topic2, sizeof(c.temp_topic2));
  cJSON *ra = cJSON_GetObjectItem(j, "relays");
  if (cJSON_IsArray(ra)) {
    for (int i = 0; i < c.relay_count; i++) {
      cJSON *o = cJSON_GetArrayItem(ra, i);
      if (!o)
        continue;
      jcpy(o, "pub", c.relay[i].pub_topic, sizeof(c.relay[i].pub_topic));
      jcpy(o, "state", c.relay[i].state_pub_topic,
           sizeof(c.relay[i].state_pub_topic));
      jcpy(o, "sub", c.relay[i].sub_topic, sizeof(c.relay[i].sub_topic));
      jcpy(o, "acs", c.relay[i].acs_topic, sizeof(c.relay[i].acs_topic));
      x = cJSON_GetObjectItem(o, "ttl");
      if (cJSON_IsNumber(x))
        c.relay[i].ttl_sec = x->valueint;
      x = cJSON_GetObjectItem(o, "tta");
      if (cJSON_IsNumber(x))
        c.relay[i].tta_sec = x->valueint;
    }
  }
  cJSON *ta = cJSON_GetObjectItem(j, "timers");
  if (cJSON_IsArray(ta)) {
    for (int i = 0; i < SC_MAX_TIMERS; i++) {
      cJSON *o = cJSON_GetArrayItem(ta, i);
      if (!o)
        continue;
      x = cJSON_GetObjectItem(o, "enabled");
      if (x)
        c.timer[i].enabled = jtruth(x);
      x = cJSON_GetObjectItem(o, "type");
      if (cJSON_IsNumber(x))
        c.timer[i].type = x->valueint;
      x = cJSON_GetObjectItem(o, "relay");
      if (cJSON_IsNumber(x))
        c.timer[i].relay = x->valueint;
      x = cJSON_GetObjectItem(o, "weekdays");
      if (cJSON_IsNumber(x))
        c.timer[i].weekdays = x->valueint;
      x = cJSON_GetObjectItem(o, "month_day");
      if (cJSON_IsNumber(x))
        c.timer[i].month_day = x->valueint;
      x = cJSON_GetObjectItem(o, "hour");
      if (cJSON_IsNumber(x))
        c.timer[i].start_hour = x->valueint;
      x = cJSON_GetObjectItem(o, "minute");
      if (cJSON_IsNumber(x))
        c.timer[i].start_minute = x->valueint;
      x = cJSON_GetObjectItem(o, "duration");
      if (cJSON_IsNumber(x))
        c.timer[i].duration_sec = x->valueint;
      x = cJSON_GetObjectItem(o, "epoch");
      if (cJSON_IsNumber(x))
        c.timer[i].once_epoch = x->valuedouble;
      jcpy(o, "date_from", c.timer_schedule[i].date_from,
           sizeof(c.timer_schedule[i].date_from));
      jcpy(o, "date_to", c.timer_schedule[i].date_to,
           sizeof(c.timer_schedule[i].date_to));
      x = cJSON_GetObjectItem(o, "end_hour");
      if (cJSON_IsNumber(x))
        c.timer_schedule[i].end_hour = x->valueint;
      x = cJSON_GetObjectItem(o, "end_minute");
      if (cJSON_IsNumber(x))
        c.timer_schedule[i].end_minute = x->valueint;
      x = cJSON_GetObjectItem(o, "duration_hours");
      if (cJSON_IsNumber(x))
        c.timer_schedule[i].duration_hours = x->valueint;
      x = cJSON_GetObjectItem(o, "duration_minutes");
      if (cJSON_IsNumber(x))
        c.timer_schedule[i].duration_minutes = x->valueint;
    }
  }
  cJSON *ia = cJSON_GetObjectItem(j, "inputs");
  if (cJSON_IsArray(ia)) {
    for (int i = 0; i < c.input_count && i < SC_MAX_INPUTS; i++) {
      cJSON *o = cJSON_GetArrayItem(ia, i);
      if (!o)
        continue;
      x = cJSON_GetObjectItem(o, "mode");
      if (cJSON_IsNumber(x) && x->valueint >= SC_INPUT_NONE &&
          x->valueint <= SC_INPUT_TEMPERATURE)
        c.input[i].mode = x->valueint;
      x = cJSON_GetObjectItem(o, "relay");
      if (cJSON_IsNumber(x))
        c.input[i].relay = x->valueint;
      jcpy(o, "topic", c.input[i].topic, sizeof(c.input[i].topic));
      if (c.input[i].relay >= c.relay_count)
        c.input[i].relay = -1;
    }
  }
  cJSON *tc = cJSON_GetObjectItem(j, "temperatureControl");
  if (cJSON_IsObject(tc)) {
    x = cJSON_GetObjectItem(tc, "enabled");
    if (x)
      c.temp_control.enabled = jtruth(x);
    x = cJSON_GetObjectItem(tc, "relay");
    if (cJSON_IsNumber(x) && x->valueint >= 0 && x->valueint < c.relay_count)
      c.temp_control.relay = x->valueint;
    x = cJSON_GetObjectItem(tc, "low");
    if (cJSON_IsNumber(x) && x->valuedouble > 0)
      c.temp_control.low_c = x->valuedouble;
    x = cJSON_GetObjectItem(tc, "high");
    if (cJSON_IsNumber(x) && x->valuedouble >= 0)
      c.temp_control.high_c = x->valuedouble;
    x = cJSON_GetObjectItem(tc, "buffer");
    if (cJSON_IsNumber(x) && x->valuedouble > 0)
      c.temp_control.buffer_c = x->valuedouble;
  }
  cJSON *aa = cJSON_GetObjectItem(j, "automation");
  if (cJSON_IsArray(aa)) {
    for (int i = 0; i < SC_MAX_AUTOMATION_RULES; i++) {
      cJSON *o = cJSON_GetArrayItem(aa, i);
      if (!cJSON_IsObject(o)) continue;
      sc_automation_rule_t rule = {0};
      sc_modbus_rule_target_t target = {.port = 502, .unit = 1,
                                        .send_clear = 1};
#define RESTORE_RULE_INT(name, field) do { x=cJSON_GetObjectItem(o,name); if(cJSON_IsNumber(x)) rule.field=x->valueint; } while(0)
#define RESTORE_RULE_FLOAT(name, field) do { x=cJSON_GetObjectItem(o,name); if(cJSON_IsNumber(x)) rule.field=(float)x->valuedouble; } while(0)
      x = cJSON_GetObjectItem(o, "enabled");
      rule.enabled = jtruth(x);
      RESTORE_RULE_INT("target", target_relay);
      RESTORE_RULE_INT("action", action);
      RESTORE_RULE_INT("logic", logic);
      RESTORE_RULE_INT("trigger", trigger_mode);
      RESTORE_RULE_INT("gate", gate_timer);
      RESTORE_RULE_INT("hold", hold_sec);
      RESTORE_RULE_INT("cooldown", cooldown_sec);
      RESTORE_RULE_INT("c0source", condition[0].source);
      RESTORE_RULE_INT("c0op", condition[0].op);
      RESTORE_RULE_FLOAT("c0v1", condition[0].value1);
      RESTORE_RULE_FLOAT("c0v2", condition[0].value2);
      RESTORE_RULE_INT("c1source", condition[1].source);
      RESTORE_RULE_INT("c1op", condition[1].op);
      RESTORE_RULE_FLOAT("c1v1", condition[1].value1);
      RESTORE_RULE_FLOAT("c1v2", condition[1].value2);
#undef RESTORE_RULE_INT
#undef RESTORE_RULE_FLOAT
      x = cJSON_GetObjectItem(o, "output");
      target.remote = cJSON_IsNumber(x) && x->valueint == 1;
      if (target.remote) {
        struct in_addr ip;
        char host[16] = {0};
        jcpy(o, "mbHost", host, sizeof(host));
        x = cJSON_GetObjectItem(o, "mbPort");
        int port = cJSON_IsNumber(x) ? x->valueint : 0;
        x = cJSON_GetObjectItem(o, "mbUnit");
        int unit = cJSON_IsNumber(x) ? x->valueint : 0;
        x = cJSON_GetObjectItem(o, "mbAddress");
        int address = cJSON_IsNumber(x) ? x->valueint : -1;
        x = cJSON_GetObjectItem(o, "mbClear");
        target.send_clear = jtruth(x);
        if (!valid_modbus_host(host) || !inet_aton(host, &ip) ||
            port < 1 || port > 65535 || unit < 1 || unit > 247 ||
            address < 0 || address > 65535) rule.enabled = 0;
        else {
          target.ipv4 = ip.s_addr;
          target.port = port;
          target.unit = unit;
          target.address = address;
        }
      }
      if ((!target.remote && rule.target_relay >= c.relay_count) ||
          rule.action > SC_AUTOMATION_ACTION_TOGGLE ||
          rule.logic > SC_AUTOMATION_LOGIC_OR ||
          rule.trigger_mode > SC_AUTOMATION_PERIODIC ||
          rule.gate_timer > SC_MAX_TIMERS ||
          !automation_source_valid(rule.condition[0].source) ||
          !automation_source_valid(rule.condition[1].source) ||
          rule.condition[0].op > SC_AUTOMATION_EQUAL ||
          rule.condition[1].op > SC_AUTOMATION_EQUAL ||
          (rule.enabled &&
           rule.condition[0].source == SC_AUTOMATION_SOURCE_NONE) ||
          (rule.condition[0].op == SC_AUTOMATION_BETWEEN &&
           rule.condition[0].value2 < rule.condition[0].value1) ||
          (rule.condition[1].source != SC_AUTOMATION_SOURCE_NONE &&
           rule.condition[1].op == SC_AUTOMATION_BETWEEN &&
           rule.condition[1].value2 < rule.condition[1].value1) ||
          ((rule.trigger_mode == SC_AUTOMATION_PERIODIC || target.remote) &&
           rule.action == SC_AUTOMATION_ACTION_TOGGLE))
        rule.enabled = 0;
      c.automation[i] = rule;
      c.modbus_rule[i] = target;
    }
  }
  cJSON *rsa = cJSON_GetObjectItem(j, "remoteSensors");
  if (cJSON_IsArray(rsa)) {
    for (int i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
      cJSON *o = cJSON_GetArrayItem(rsa, i);
      if (!cJSON_IsObject(o)) continue;
      sc_remote_sensor_config_t remote = {0};
      jcpy(o, "label", remote.label, sizeof(remote.label));
      x = cJSON_GetObjectItem(o, "transport");
      bool modbus = cJSON_IsNumber(x) && x->valueint == SC_REMOTE_MODBUS_TCP;
      if (modbus) {
        sc_remote_modbus_config_t m = {.scale = 1.0f};
        bool fields_valid = true;
        jcpy(o, "host", remote.topic, sizeof(remote.topic));
#define RESTORE_MB_INT(name, field, low, high) do { \
          x=cJSON_GetObjectItem(o,name); \
          if(!cJSON_IsNumber(x)||x->valuedouble<(low)|| \
             x->valuedouble>(high)||floor(x->valuedouble)!=x->valuedouble) \
            fields_valid=false; \
          else m.field=x->valueint; \
        } while(0)
        RESTORE_MB_INT("port", port, 1, 65535);
        RESTORE_MB_INT("unit", unit, 1, 247);
        RESTORE_MB_INT("function", function, 1, 4);
        RESTORE_MB_INT("address", address, 0, 65535);
        RESTORE_MB_INT("valueType", value_type, 0, SC_MODBUS_F32);
        RESTORE_MB_INT("wordOrder", word_order, 0, 3);
        RESTORE_MB_INT("poll", poll_sec, 5, SC_REMOTE_STALE_SEC);
#undef RESTORE_MB_INT
        x = cJSON_GetObjectItem(o, "scale");
        if (cJSON_IsNumber(x)) m.scale = x->valuedouble;
        else fields_valid = false;
        x = cJSON_GetObjectItem(o, "offset");
        if (cJSON_IsNumber(x)) m.offset = x->valuedouble;
        else fields_valid = false;
        if (fields_valid && valid_modbus_settings(remote.topic, &m)) {
          sc_remote_modbus_encode(&remote, &m);
          c.remote_transport[i] = SC_REMOTE_MODBUS_TCP;
        } else {
          memset(&remote, 0, sizeof(remote));
          c.remote_transport[i] = SC_REMOTE_MQTT;
        }
      } else {
        jcpy(o, "topic", remote.topic, sizeof(remote.topic));
        jcpy(o, "jsonKey", remote.json_key, sizeof(remote.json_key));
        c.remote_transport[i] = SC_REMOTE_MQTT;
        if (remote.topic[0] && !valid_publish_topic(remote.topic))
          memset(&remote, 0, sizeof(remote));
        for (int previous = 0; remote.topic[0] && previous < i; previous++)
          if (c.remote_transport[previous] == SC_REMOTE_MQTT &&
              !strcmp(remote.topic, c.remote_sensor[previous].topic))
            memset(&remote, 0, sizeof(remote));
      }
      c.remote_sensor[i] = remote;
    }
  }
  if (!c.web_user[0] || strchr(c.web_user, ':') || !c.web_password[0])
    return ESP_ERR_INVALID_ARG;
  if (!sc_sensors_validate_config(&c)) return ESP_ERR_INVALID_ARG;
  esp_err_t saved = sc_config_replace(&c);
  if (saved == ESP_OK) {
    for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
      sc_automation_reset_remote(i);
      sc_modbus_remote_changed(i);
      if (c.remote_transport[i] == SC_REMOTE_MQTT)
        sc_mqtt_update_remote_subscription(NULL, c.remote_sensor[i].topic);
    }
    sc_modbus_ensure_started();
    saved = sc_sensors_ensure_started();
  }
  return saved;
}

static int b64_value(uint8_t c){if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;if(c=='=')return -2;return -1;}
static int b64_decode4(const uint8_t in[4],uint8_t out[3]){int a=b64_value(in[0]),b=b64_value(in[1]),c=b64_value(in[2]),d=b64_value(in[3]);if(a<0||b<0||c==-1||d==-1)return -1;uint32_t v=((uint32_t)a<<18)|((uint32_t)b<<12)|((uint32_t)(c<0?0:c)<<6)|(uint32_t)(d<0?0:d);out[0]=(uint8_t)(v>>16);if(c<0)return 1;out[1]=(uint8_t)(v>>8);if(d<0)return 2;out[2]=(uint8_t)v;return 3;}
static esp_err_t backup_chunk(httpd_req_t*r,const void*data,size_t len){return len?httpd_resp_send_chunk(r,data,len):ESP_OK;}
static esp_err_t backup_b64_memory(httpd_req_t*r,const void*data,size_t len){const uint8_t*p=data;char out[256];while(len){size_t take=len>192?192:len;size_t n=b64_encode(p,take,out);if(backup_chunk(r,out,n)!=ESP_OK)return ESP_FAIL;p+=take;len-=take;}return ESP_OK;}
static esp_err_t backup_entry_memory(httpd_req_t*r,const char*name,const void*data,size_t len,bool comma){char head[96];int n=snprintf(head,sizeof(head),"%s\"%s\":\"",comma?",":"",name);if(n<0||n>=(int)sizeof(head)||backup_chunk(r,head,n)!=ESP_OK||backup_b64_memory(r,data,len)!=ESP_OK)return ESP_FAIL;return backup_chunk(r,"\"",1);}
static esp_err_t backup_entry_file(httpd_req_t*r,const char*name,bool comma){char head[96],raw[192],encoded[256];int hn=snprintf(head,sizeof(head),"%s\"%s\":\"",comma?",":"",name);if(hn<0||hn>=(int)sizeof(head)||backup_chunk(r,head,hn)!=ESP_OK)return ESP_FAIL;if(sc_webfs_read_stream_open(name)<0)return ESP_FAIL;esp_err_t e=ESP_OK;size_t n=0;unsigned chunks=0;do{if(sc_webfs_read_stream_read(raw,sizeof(raw),&n)<0){e=ESP_FAIL;break;}if(n){size_t enc=b64_encode((const uint8_t*)raw,n,encoded);if(backup_chunk(r,encoded,enc)!=ESP_OK){e=ESP_FAIL;break;}if(++chunks%16==0)vTaskDelay(1);}}while(n);if(sc_webfs_read_stream_close()<0)e=ESP_FAIL;if(e==ESP_OK)e=backup_chunk(r,"\"",1);return e;}
static esp_err_t backup_get(httpd_req_t*r){char q[64],value[8],node[13];bool include_web=false;if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK&&httpd_query_key_value(q,"includeWeb",value,sizeof(value))==ESP_OK)include_web=atoi(value)!=0;size_t count=0,file_count=0;if(include_web){if(sc_webfs_count(&count))return send_error(r,HTTPD_500,"filesystem scan failed");sc_lfs_entry_t entry;for(size_t i=0;i<count;i++){if(sc_webfs_entry_at(i,&entry))return send_error(r,HTTPD_500,"filesystem scan failed");if(!entry.is_dir&&strcmp(entry.name,"/config.json"))file_count++;}}cJSON*cfg=config_json(true);char*cfg_text=cfg?cJSON_PrintUnformatted(cfg):NULL;cJSON_Delete(cfg);if(!cfg_text)return send_error(r,HTTPD_500,"out of memory");controller_id(node);char head[160];int n=snprintf(head,sizeof(head),"{\"_backup_version\":1,\"_chipid\":\"%s\",\"_includesWeb\":%s,\"_files\":%u,",node,include_web?"true":"false",(unsigned)(file_count+1));httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Content-Disposition","attachment; filename=\"smartconfig-backup.json\"");httpd_resp_set_hdr(r,"Connection","close");esp_err_t e=(n>0&&n<(int)sizeof(head))?backup_chunk(r,head,n):ESP_FAIL;if(e==ESP_OK)e=backup_entry_memory(r,"/config.json",cfg_text,strlen(cfg_text),false);free(cfg_text);for(size_t i=0;e==ESP_OK&&i<count;i++){sc_lfs_entry_t entry;if(sc_webfs_entry_at(i,&entry)){e=ESP_FAIL;break;}if(!entry.is_dir&&strcmp(entry.name,"/config.json")){e=backup_entry_file(r,entry.name,true);vTaskDelay(1);}}if(e==ESP_OK)e=backup_chunk(r,"}",1);if(e==ESP_OK)e=httpd_resp_send_chunk(r,NULL,0);return e;}

typedef enum{REST_KEY_START,REST_KEY,REST_COLON,REST_VALUE_START,REST_META_STRING,REST_META_BARE,REST_FILE_STRING,REST_COMMA,REST_DONE}restore_phase_t;
typedef struct{restore_phase_t phase;bool restore_web,saw_version,includes_web,failed,file_skip,file_config,file_open;char chip_mode[12],old_id[16],key[SC_LFS_NAME_MAX],meta[32];size_t key_len,meta_len;uint8_t group[4],group_len,out[192];size_t out_len,config_len;char*config;unsigned written;}restore_state_t;
static void normalize_controller_ids(char*text,const char*target){if(!text||!target||strlen(target)!=12)return;for(char*p=text;(p=strstr(p,"Controller"))!=NULL;p+=10){char*id=p+10;bool valid=strlen(id)>=12;for(int i=0;valid&&i<12;i++)if(!isxdigit((unsigned char)id[i]))valid=false;if(valid){memcpy(id,target,12);p=id+12;}}}
static void restore_flush(restore_state_t*s){if(!s->file_open||!s->out_len)return;if(sc_webfs_stream_write(s->out,s->out_len)<0)s->failed=true;s->out_len=0;}
static void restore_emit(restore_state_t*s,const uint8_t*data,size_t len){if(s->file_skip||s->failed)return;if(s->file_config){if(!s->config||s->config_len+len>=6144){s->failed=true;return;}memcpy(s->config+s->config_len,data,len);s->config_len+=len;return;}while(len){size_t room=sizeof(s->out)-s->out_len,take=len<room?len:room;memcpy(s->out+s->out_len,data,take);s->out_len+=take;data+=take;len-=take;if(s->out_len==sizeof(s->out))restore_flush(s);}}
static void restore_begin_file(restore_state_t*s){s->file_config=!strcmp(s->key,"/config.json");s->file_skip=s->restore_web?s->file_config:!s->file_config;s->group_len=0;s->out_len=0;s->file_open=false;if(s->file_skip)return;if(s->file_config){s->config=malloc(6144);if(!s->config)s->failed=true;}else if(!safe_path(s->key)||sc_webfs_stream_open(s->key)<0)s->failed=true;else s->file_open=true;}
static void restore_finish_file(restore_state_t*s){if(s->group_len)s->failed=true;if(s->file_config&&s->config)s->config[s->config_len]=0;if(s->file_open){restore_flush(s);sc_webfs_stream_close();s->file_open=false;if(!s->failed)s->written++;}}
static void restore_meta_done(restore_state_t*s){s->meta[s->meta_len]=0;if(!strcmp(s->key,"_backup_version"))s->saw_version=true;else if(!strcmp(s->key,"_chipid")){size_t n=strnlen(s->meta,12);memcpy(s->old_id,s->meta,n);s->old_id[n]=0;}else if(!strcmp(s->key,"_includesWeb"))s->includes_web=!strcmp(s->meta,"true");}
static void restore_b64_char(restore_state_t*s,uint8_t c){if(s->file_skip||s->failed)return;s->group[s->group_len++]=c;if(s->group_len==4){uint8_t raw[3];int n=b64_decode4(s->group,raw);s->group_len=0;if(n<0)s->failed=true;else restore_emit(s,raw,(size_t)n);}}
static void restore_byte(restore_state_t*s,char c){switch(s->phase){case REST_KEY_START:if(c=='\"'){s->key_len=0;s->phase=REST_KEY;}else if(c=='}')s->phase=REST_DONE;break;case REST_KEY:if(c=='\"'){s->key[s->key_len]=0;s->phase=REST_COLON;}else if(s->key_len+1<sizeof(s->key))s->key[s->key_len++]=c;else s->failed=true;break;case REST_COLON:if(c==':')s->phase=REST_VALUE_START;break;case REST_VALUE_START:if(isspace((unsigned char)c))break;s->meta_len=0;if(c=='\"'){if(s->key[0]=='_')s->phase=REST_META_STRING;else{restore_begin_file(s);s->phase=REST_FILE_STRING;}}else{s->meta[s->meta_len++]=c;s->phase=REST_META_BARE;}break;case REST_META_STRING:if(c=='\"'){restore_meta_done(s);s->phase=REST_COMMA;}else if(s->meta_len+1<sizeof(s->meta))s->meta[s->meta_len++]=c;else s->failed=true;break;case REST_META_BARE:if(c==','||c=='}'){while(s->meta_len&&isspace((unsigned char)s->meta[s->meta_len-1]))s->meta_len--;restore_meta_done(s);s->phase=c=='}'?REST_DONE:REST_KEY_START;}else if(s->meta_len+1<sizeof(s->meta))s->meta[s->meta_len++]=c;else s->failed=true;break;case REST_FILE_STRING:if(c=='\"'){restore_finish_file(s);s->phase=REST_COMMA;}else restore_b64_char(s,(uint8_t)c);break;case REST_COMMA:if(c==',')s->phase=REST_KEY_START;else if(c=='}')s->phase=REST_DONE;break;case REST_DONE:break;}}
static esp_err_t backup_restore(httpd_req_t*r){restore_state_t s={.phase=REST_KEY_START};char q[128],v[16];if(httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK){if(httpd_query_key_value(q,"restoreWeb",v,sizeof(v))==ESP_OK)s.restore_web=atoi(v)!=0;if(httpd_query_key_value(q,"chipIdMode",s.chip_mode,sizeof(s.chip_mode))!=ESP_OK)snprintf(s.chip_mode,sizeof(s.chip_mode),"keep");}char input[512];int left=r->content_len;while(left>0&&!s.failed){int want=left>(int)sizeof(input)?sizeof(input):left;int n=httpd_req_recv(r,input,want);if(n<=0){s.failed=true;break;}for(int i=0;i<n&&!s.failed;i++)restore_byte(&s,input[i]);left-=n;}if(s.file_open){sc_webfs_stream_close();s.file_open=false;}bool ok=!s.failed&&s.saw_version&&s.phase==REST_DONE;if(ok&&s.restore_web)ok=s.includes_web&&s.written>0;if(ok&&!s.restore_web){if(!s.config)ok=false;else{char target[13]={0};if(!strcmp(s.chip_mode,"replace"))controller_id(target);else if(!strcmp(s.chip_mode,"original")&&strlen(s.old_id)==12){memcpy(target,s.old_id,12);target[12]=0;}if(target[0])normalize_controller_ids(s.config,target);cJSON*j=cJSON_Parse(s.config);if(!j)ok=false;else{ok=restore_config_object(j)==ESP_OK;cJSON_Delete(j);}}}free(s.config);char message[128];if(ok&&s.restore_web)snprintf(message,sizeof(message),"%u web file(s) restored",s.written);else if(ok)snprintf(message,sizeof(message),"Configuration restored");else snprintf(message,sizeof(message),"Restore failed or backup is not compatible");cJSON*out=cJSON_CreateObject();cJSON_AddBoolToObject(out,"ok",ok);cJSON_AddStringToObject(out,"message",message);esp_err_t response=js(r,out);if(ok)schedule_reboot();return response;}
static esp_err_t reset_config(httpd_req_t *r) {
  sc_config_t defaults;
  sc_config_defaults(&defaults);
  const sc_config_t *old = sc_config_acquire();
  for (int i = 0; i < SC_MAX_TIMERS; i++) {
    defaults.timer[i] = old->timer[i];
    defaults.timer[i].enabled = 0;
    defaults.timer_schedule[i] = old->timer_schedule[i];
  }
  for (int i = 0; i < SC_MAX_AUTOMATION_RULES; i++) {
    defaults.automation[i] = old->automation[i];
    defaults.automation[i].enabled = 0;
    defaults.modbus_rule[i] = old->modbus_rule[i];
  }
  memcpy(defaults.remote_sensor, old->remote_sensor, sizeof(defaults.remote_sensor));
  memcpy(defaults.remote_transport, old->remote_transport, sizeof(defaults.remote_transport));
  sc_config_release();
  bool ok = sc_config_replace(&defaults) == ESP_OK;
  if (ok) for (int i = 0; i < SC_MAX_AUTOMATION_RULES; i++)
    sc_modbus_rule_submit(i, NULL, false);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "ok", ok);
  cJSON_AddStringToObject(j, "message", ok
    ? "All configuration reset; timers and automation retained but disabled" : "Reset failed");
  esp_err_t e = js(r, j);
  if (ok) schedule_reboot();
  return e;
}

static esp_err_t files_result(httpd_req_t*r,bool ok,const char*message){cJSON*j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",ok);cJSON_AddStringToObject(j,"message",message);return js(r,j);}
static esp_err_t files_list(httpd_req_t*r){size_t count=0;if(sc_webfs_count(&count))return send_error(r,HTTPD_500,"filesystem scan failed");httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");if(httpd_resp_send_chunk(r,"{\"files\":[",10)!=ESP_OK)return ESP_FAIL;for(size_t i=0;i<count;i++){sc_lfs_entry_t entry;if(sc_webfs_entry_at(i,&entry))return ESP_FAIL;char name[SC_LFS_NAME_MAX*6+1],row[SC_LFS_NAME_MAX*6+96];json_escape(name,sizeof(name),entry.name);int n=snprintf(row,sizeof(row),"%s{\"name\":\"%s\",\"size\":%u,\"dir\":%s}",i?",":"",name,(unsigned)entry.size,entry.is_dir?"true":"false");if(n<0||n>=(int)sizeof(row)||httpd_resp_send_chunk(r,row,n)!=ESP_OK)return ESP_FAIL;}char tail[96];int n=snprintf(tail,sizeof(tail),"],\"fsTotal\":%u,\"fsUsed\":%u}",(unsigned)sc_webfs_total(),(unsigned)sc_webfs_used());if(n<0||n>=(int)sizeof(tail)||httpd_resp_send_chunk(r,tail,n)!=ESP_OK)return ESP_FAIL;return httpd_resp_send_chunk(r,NULL,0);}
static esp_err_t files_content_get(httpd_req_t*r){char q[128],p[SC_LFS_NAME_MAX];if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK||httpd_query_key_value(q,"path",p,sizeof(p))!=ESP_OK){send_error(r,HTTPD_400,"bad path");return ESP_FAIL;}form_url_decode(p);if(!safe_path(p)){send_error(r,HTTPD_400,"bad path");return ESP_FAIL;}httpd_resp_set_type(r,"text/plain");httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"Connection","close");char b[768];size_t off=0,n=0;do{if(sc_webfs_read_range(p,off,b,sizeof(b),&n)){if(!off)send_error(r,HTTPD_404,"not found");return ESP_FAIL;}if(n){if(httpd_resp_send_chunk(r,b,n)!=ESP_OK)return ESP_FAIL;off+=n;}}while(n==sizeof(b));return httpd_resp_send_chunk(r,NULL,0);}
static esp_err_t files_content_post(httpd_req_t*r){char q[128],p[SC_LFS_NAME_MAX];if(httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK||httpd_query_key_value(q,"path",p,sizeof(p))!=ESP_OK)return files_result(r,false,"Invalid path");form_url_decode(p);if(!safe_path(p))return files_result(r,false,"Invalid path");if(sc_webfs_stream_open(p)<0)return files_result(r,false,"Could not open file");char b[768];int left=r->content_len,written=0;bool ok=true;while(left>0){int want=left>(int)sizeof(b)?sizeof(b):left;int n=httpd_req_recv(r,b,want);if(n<=0||sc_webfs_stream_write(b,n)<0){ok=false;break;}left-=n;written+=n;}sc_webfs_stream_close();char msg[96];if(ok)snprintf(msg,sizeof(msg),"%d byte(s) written to %s",written,p);else snprintf(msg,sizeof(msg),"Write failed for %s",p);return files_result(r,ok,msg);}
static esp_err_t files_delete(httpd_req_t*r){char body[128],p[SC_LFS_NAME_MAX]={0};if(recv_body(r,body,sizeof(body))!=ESP_OK)return ESP_FAIL;qstr(body,"path",p,sizeof(p));if(!safe_path(p))return files_result(r,false,"Invalid path");bool ok=sc_webfs_remove(p)==0;char msg[96];snprintf(msg,sizeof(msg),ok?"Deleted %s":"Could not delete %s",p);return files_result(r,ok,msg);}
static esp_err_t files_rename(httpd_req_t*r){char body[192],from[SC_LFS_NAME_MAX]={0},to[SC_LFS_NAME_MAX]={0};if(recv_body(r,body,sizeof(body))!=ESP_OK)return ESP_FAIL;qstr(body,"from",from,sizeof(from));qstr(body,"to",to,sizeof(to));if(!safe_path(from)||!safe_path(to))return files_result(r,false,"Invalid source or destination path");bool ok=sc_webfs_rename(from,to)==0;char msg[96];snprintf(msg,sizeof(msg),ok?"Renamed to %s":"Rename failed",to);return files_result(r,ok,msg);}

static esp_err_t core_status(httpd_req_t*r){sc_diag_t d;sc_diag_snapshot(&d);char body[192];int n=snprintf(body,sizeof(body),"{\"hasDump\":%s,\"task\":\"unknown\",\"excCause\":%u,\"epc1\":0,\"reason\":\"%s\",\"bootCount\":%u}",d.has_dump?"true":"false",(unsigned)d.reset_reason,d.reason,(unsigned)d.boot_count);if(n<0||n>=(int)sizeof(body))return send_error(r,HTTPD_500,"diagnostic response too large");return json_text(r,body);}
static esp_err_t core_erase(httpd_req_t*r){esp_err_t e=sc_diag_clear();cJSON*j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"ok",e==ESP_OK);return js(r,j);}
static esp_err_t ota_upload(httpd_req_t*r){if(sc_ota_begin(r->content_len)!=ESP_OK){send_error(r,HTTPD_500,"ota begin failed");return ESP_FAIL;}char b[1024];int left=r->content_len;while(left>0){int want=left>(int)sizeof(b)?sizeof(b):left;int n=httpd_req_recv(r,b,want);if(n<=0||sc_ota_write(b,n)!=ESP_OK){sc_ota_abort();send_error(r,HTTPD_500,"ota write failed");return ESP_FAIL;}left-=n;}if(sc_ota_finish()!=ESP_OK){send_error(r,HTTPD_500,"ota validation failed");return ESP_FAIL;}httpd_resp_sendstr(r,"OK - rebooting");schedule_reboot();return ESP_OK;}

static esp_err_t static_file(httpd_req_t *r)
{
  char path[SC_LFS_NAME_MAX];
  size_t path_len = strcspn(r->uri, "?#");
  if (!path_len || path_len >= sizeof(path))
    return send_error(r, HTTPD_404, "not found");
  memcpy(path, r->uri, path_len);
  path[path_len] = 0;
  const char *p = path;
  if (!strcmp(p, "/")) p = "/index.html";
  else if (!strcmp(p, "/Timer1")) p = "/Timer1.html";
  else if (!strcmp(p, "/Input_Relays_Map")) p = "/Input_Relays_Map.html";
  else if (!strcmp(p, "/Automation")) p = "/Automation.html";

  char b[256], compressed[SC_LFS_NAME_MAX];
  size_t n = 0;
  bool gzip = false;
  if (sc_webfs_read_stream_open(p) < 0) {
    int len = snprintf(compressed, sizeof(compressed), "%s.gz", p);
    if (len < 0 || len >= (int)sizeof(compressed) ||
        sc_webfs_read_stream_open(compressed) < 0)
      return send_error(r, HTTPD_404, "not found");
    gzip = true;
  }

  httpd_resp_set_hdr(r, "Connection", "close");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  const char *extension = strrchr(p, '.');
  if (extension && !strcmp(extension, ".html"))
    httpd_resp_set_type(r, "text/html");
  else if (extension && !strcmp(extension, ".css"))
    httpd_resp_set_type(r, "text/css");
  else if (extension && !strcmp(extension, ".js"))
    httpd_resp_set_type(r, "application/javascript");
  if (gzip) {
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  }
  esp_err_t result = ESP_OK;
  for (;;) {
    if (sc_webfs_read_stream_read(b, sizeof(b), &n) < 0) {
      result = ESP_FAIL;
      break;
    }
    if (!n) break;
    if (httpd_resp_send_chunk(r, b, n) != ESP_OK) {
      result = ESP_FAIL;
      break;
    }
  }
  if (sc_webfs_read_stream_close() < 0) result = ESP_FAIL;
  return result == ESP_OK ? httpd_resp_send_chunk(r, NULL, 0) : result;
}

esp_err_t sc_web_init(void){
 // Saves keep a full config snapshot live through LittleFS metadata compaction.
 // Include flash/interrupt headroom; tools/test_http_stack.py checks build reports.
 httpd_config_t c=HTTPD_DEFAULT_CONFIG();
 c.max_open_sockets=2;c.max_uri_handlers=48;c.stack_size=9472;
 c.lru_purge_enable=true;c.recv_wait_timeout=5;c.send_wait_timeout=5;
 ESP_ERROR_CHECK(httpd_start(&server,&c));
 static const sc_web_route_t routes[]={
 {"/api/heap",HTTP_GET,heap},{"/api/restart",HTTP_POST,restart_device},{"/GetConfig.json",HTTP_GET,getcfg},{"/FeatureFlags.json",HTTP_GET,feature_flags},{"/SensorStatus.json",HTTP_GET,sensors},{"/WifiStatus.json",HTTP_GET,wifi_status},{"/MqttStatus.json",HTTP_GET,mqtt_status},{"/WireGuardStatus.json",HTTP_GET,wireguard_status},{"/RelayControl.json",HTTP_GET,relayctl},{"/RelayState.json",HTTP_GET,relaystate},{"/InputConfig.json",HTTP_GET,input_config},{"/ApplyInputs.html",HTTP_POST,apply_inputs},{"/TemperatureConfig.json",HTTP_GET,temperature_config},{"/AdvancedTempSave.html",HTTP_POST,apply_temperature},{"/Apply.html",HTTP_GET,apply},{"/ApplyRelay.html",HTTP_GET,applyrelay},{"/TimerStatus.json",HTTP_GET,timer_status},{"/TimerControl.json",HTTP_GET,timer_control},{"/api/timer/save",HTTP_GET,timer_save},{"/savetimer.html",HTTP_GET,timer_save_form},{"/api/automation",HTTP_GET,automation_get},{"/api/automation/rule",HTTP_POST,automation_rule_save},{"/api/automation/remote",HTTP_POST,automation_remote_save},{"/api/backup",HTTP_GET,backup_get},{"/api/restore",HTTP_POST,backup_restore},{"/api/resetconfig",HTTP_POST,reset_config},{"/api/files/list",HTTP_GET,files_list},{"/api/files/content",HTTP_GET,files_content_get},{"/api/files/content",HTTP_POST,files_content_post},{"/api/files/delete",HTTP_POST,files_delete},{"/api/files/rename",HTTP_POST,files_rename},{"/CoreDumpStatus.json",HTTP_GET,core_status},{"/EraseCoreDump.json",HTTP_POST,core_erase},{"/api/ota",HTTP_POST,ota_upload},
 {"/",HTTP_GET,static_file},{"/config.html",HTTP_GET,static_file},{"/LiveReadings.html",HTTP_GET,static_file},{"/RelayConfig.html",HTTP_GET,static_file},{"/Input_Relays_Map",HTTP_GET,static_file},{"/Input_Relays_Map.html",HTTP_GET,static_file},{"/AdvancedTemperatureConf.html",HTTP_GET,static_file},{"/Timer1",HTTP_GET,timer_page},{"/Timer1.html",HTTP_GET,timer_page},{"/Automation",HTTP_GET,static_file},{"/Files.html",HTTP_GET,static_file},{"/Backup.html",HTTP_GET,static_file},{"/coredump.html",HTTP_GET,static_file},{"/FirmwareMaintenance.html",HTTP_GET,static_file}};
 for(unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);i++){httpd_uri_t u={.uri=routes[i].uri,.method=routes[i].method,.handler=auth_dispatch,.user_ctx=(void*)&routes[i]};esp_err_t e=httpd_register_uri_handler(server,&u);if(e!=ESP_OK)return e;}return ESP_OK;
}
