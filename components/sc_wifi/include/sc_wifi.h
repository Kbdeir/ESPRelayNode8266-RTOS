#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct {bool connected; char ip[16],gateway[16],dns1[16],dns2[16],ssid[33]; int8_t rssi;} sc_wifi_status_t;
esp_err_t sc_wifi_init(void); void sc_wifi_status(sc_wifi_status_t*out);
