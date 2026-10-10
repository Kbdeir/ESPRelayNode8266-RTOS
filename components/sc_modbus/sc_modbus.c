#include <errno.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sc_automation.h"
#include "sc_config.h"
#include "sc_relay.h"
#include "sc_modbus.h"
#include "sc_wifi.h"

#define MODBUS_ADU_MAX 128

static uint8_t modbus_unit_id = 1;
static bool task_running;
static uint8_t poll_cursor;
static int64_t next_poll_us[SC_MAX_REMOTE_SENSORS];
static uint16_t transaction_id;
static const char *TAG = "modbus";
typedef struct {
    sc_modbus_rule_target_t target;
    int64_t due_us;
    uint32_t revision;
    bool pending;
    bool on;
} rule_write_t;
static rule_write_t rule_write[SC_MAX_AUTOMATION_RULES];
static int send_all(int socket_fd, const uint8_t *data, int length);

bool sc_modbus_rule_submit(uint8_t index,
    const sc_modbus_rule_target_t *target, bool on)
{
    if (index >= SC_MAX_AUTOMATION_RULES) return false;
    taskENTER_CRITICAL();
    rule_write_t *slot = &rule_write[index];
    if (!target) {
        slot->pending = false;
        slot->revision++;
    } else if (!slot->pending || slot->on != on ||
               memcmp(&slot->target, target, sizeof(*target))) {
        slot->target = *target;
        slot->on = on;
        slot->pending = true;
        slot->due_us = 0;
        slot->revision++;
    }
    taskEXIT_CRITICAL();
    return true;
}

void sc_modbus_remote_changed(uint8_t index)
{
    if (index >= SC_MAX_REMOTE_SENSORS) return;
    taskENTER_CRITICAL();
    next_poll_us[index] = 0;
    taskEXIT_CRITICAL();
}

static bool has_remote_sensors(const sc_config_t *cfg)
{
    for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++)
        if (cfg->remote_transport[i] == SC_REMOTE_MODBUS_TCP &&
            cfg->remote_sensor[i].topic[0]) return true;
    return false;
}

static bool has_rule_writes(const sc_config_t *cfg)
{
    for (uint8_t i = 0; i < SC_MAX_AUTOMATION_RULES; i++)
        if (cfg->automation[i].enabled && cfg->modbus_rule[i].remote)
            return true;
    return false;
}

static bool read_exact(int fd, uint8_t *out, size_t length)
{
    while (length) {
        int n = recv(fd, out, length, 0);
        if (n <= 0) return false;
        out += n;
        length -= (size_t)n;
    }
    return true;
}

static bool read_modbus_value(int fd, const sc_remote_modbus_config_t *settings,
                              float *out)
{
    uint8_t request[12] = {0};
    uint8_t response[16];
    uint16_t id = ++transaction_id;
    uint16_t count = settings->value_type >= SC_MODBUS_U32 ? 2 : 1;
    request[0] = id >> 8;
    request[1] = id;
    request[5] = 6;
    request[6] = settings->unit;
    request[7] = settings->function;
    request[8] = settings->address >> 8;
    request[9] = settings->address;
    request[11] = count;
    if (send_all(fd, request, sizeof(request)) < 0 ||
        !read_exact(fd, response, 7)) return false;
    uint16_t length = ((uint16_t)response[4] << 8) | response[5];
    if (response[0] != request[0] || response[1] != request[1] ||
        response[2] || response[3] || response[6] != settings->unit ||
        length < 3 || length > sizeof(response) - 6 ||
        !read_exact(fd, response + 7, length - 1)) return false;
    uint8_t *pdu = response + 7;
    uint8_t bytes = settings->function <= 2 ? 1 : count * 2;
    if (pdu[0] != settings->function || pdu[1] != bytes ||
        length != (uint16_t)(3 + bytes)) return false;
    float value;
    if (settings->function <= 2) {
        value = (pdu[2] & 1U) ? 1.0f : 0.0f;
    } else if (count == 1) {
        uint16_t raw = ((uint16_t)pdu[2] << 8) | pdu[3];
        value = settings->value_type == SC_MODBUS_I16 ? (int16_t)raw :
                settings->value_type == SC_MODBUS_BOOL ? (raw != 0) : raw;
    } else {
        uint8_t b[4] = {pdu[2], pdu[3], pdu[4], pdu[5]};
        if (settings->word_order & 1U) {
            uint8_t t = b[0]; b[0] = b[2]; b[2] = t;
            t = b[1]; b[1] = b[3]; b[3] = t;
        }
        if (settings->word_order & 2U) {
            uint8_t t = b[0]; b[0] = b[1]; b[1] = t;
            t = b[2]; b[2] = b[3]; b[3] = t;
        }
        uint32_t raw = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                       ((uint32_t)b[2] << 8) | b[3];
        if (settings->value_type == SC_MODBUS_F32) {
            memcpy(&value, &raw, sizeof(value));
        } else {
            value = settings->value_type == SC_MODBUS_I32 ? (float)(int32_t)raw
                                                         : (float)raw;
        }
    }
    value = value * settings->scale + settings->offset;
    if (!isfinite(value)) return false;
    *out = value;
    return true;
}

static int connect_remote(struct in_addr ip, uint16_t port)
{
    if (ip.s_addr == INADDR_ANY || ip.s_addr == INADDR_BROADCAST) return -1;
    sc_wifi_status_t wifi;
    sc_wifi_status(&wifi);
    struct in_addr own_ip;
    if (!wifi.connected ||
        (inet_aton(wifi.ip, &own_ip) && own_ip.s_addr == ip.s_addr))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = ip
    };
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto done;
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) goto done;
    if (result < 0) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        if (select(fd + 1, NULL, &writefds, NULL, &timeout) != 1) goto done;
        int error = 0;
        socklen_t error_len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0 || error)
            goto done;
    }
    if (fcntl(fd, F_SETFL, 0) < 0) goto done;
    return fd;
done:
    close(fd);
    return -1;
}

static bool poll_remote(uint8_t index, const sc_remote_sensor_config_t *sensor,
                        const sc_remote_modbus_config_t *settings, float *out)
{
    struct in_addr ip;
    if (!inet_aton(sensor->topic, &ip)) return false;
    int fd = connect_remote(ip, settings->port);
    if (fd < 0) return false;
    bool ok = read_modbus_value(fd, settings, out);
    if (ok) {
        int64_t now = esp_timer_get_time();
        for (uint8_t i = 0; i < SC_MAX_REMOTE_SENSORS; i++) {
            if (i == index) continue;
            int64_t due;
            taskENTER_CRITICAL();
            due = next_poll_us[i];
            taskEXIT_CRITICAL();
            if (now < due) continue;
            sc_remote_sensor_config_t other;
            sc_remote_modbus_config_t other_settings;
            const sc_config_t *cfg = sc_config_acquire();
            bool same = cfg->remote_transport[i] == SC_REMOTE_MODBUS_TCP &&
                        !strcmp(cfg->remote_sensor[i].topic, sensor->topic);
            if (same) {
                other = cfg->remote_sensor[i];
                sc_remote_modbus_decode(&other, &other_settings);
                same = other_settings.port == settings->port;
            }
            sc_config_release();
            if (!same) continue;
            taskENTER_CRITICAL();
            next_poll_us[i] = now + (int64_t)other_settings.poll_sec * 1000000LL;
            taskEXIT_CRITICAL();
            float other_value;
            bool other_ok = read_modbus_value(fd, &other_settings, &other_value);
            cfg = sc_config_acquire();
            bool unchanged = cfg->remote_transport[i] == SC_REMOTE_MODBUS_TCP &&
                             !memcmp(&cfg->remote_sensor[i], &other, sizeof(other));
            if (other_ok && unchanged) sc_automation_remote_value(i, other_value);
            sc_config_release();
            if (!unchanged) sc_modbus_remote_changed(i);
            break;
        }
    }
    close(fd);
    return ok;
}

static bool write_coil(const sc_modbus_rule_target_t *target, bool on)
{
    struct in_addr ip = {.s_addr = target->ipv4};
    int fd = connect_remote(ip, target->port);
    if (fd < 0) return false;
    uint8_t request[12] = {0}, response[12];
    uint16_t id = ++transaction_id;
    request[0] = id >> 8;
    request[1] = id;
    request[5] = 6;
    request[6] = target->unit;
    request[7] = 5;
    request[8] = target->address >> 8;
    request[9] = target->address;
    request[10] = on ? 0xff : 0;
    bool ok = send_all(fd, request, sizeof(request)) == 0 &&
              read_exact(fd, response, sizeof(response)) &&
              !memcmp(request, response, sizeof(request));
    close(fd);
    return ok;
}

static void process_rule_write(void)
{
    int64_t now = esp_timer_get_time();
    for (uint8_t i = 0; i < SC_MAX_AUTOMATION_RULES; i++) {
        rule_write_t item;
        taskENTER_CRITICAL();
        item = rule_write[i];
        taskEXIT_CRITICAL();
        if (!item.pending || now < item.due_us) continue;
        bool ok = write_coil(&item.target, item.on);
        taskENTER_CRITICAL();
        if (rule_write[i].revision == item.revision) {
            if (ok) rule_write[i].pending = false;
            else rule_write[i].due_us = esp_timer_get_time() + 5000000LL;
        }
        taskEXIT_CRITICAL();
        if (!ok) ESP_LOGW(TAG, "rule %u coil write failed; retry in 5s", i + 1);
        return;
    }
}

static void poll_one_remote(void)
{
    int64_t now = esp_timer_get_time();
    sc_remote_sensor_config_t sensor;
    sc_remote_modbus_config_t settings;
    uint8_t selected = SC_MAX_REMOTE_SENSORS;
    const sc_config_t *cfg = sc_config_acquire();
    for (uint8_t offset = 0; offset < SC_MAX_REMOTE_SENSORS; offset++) {
        uint8_t i = (poll_cursor + offset) % SC_MAX_REMOTE_SENSORS;
        int64_t due;
        taskENTER_CRITICAL();
        due = next_poll_us[i];
        taskEXIT_CRITICAL();
        if (cfg->remote_transport[i] != SC_REMOTE_MODBUS_TCP ||
            !cfg->remote_sensor[i].topic[0] || now < due) continue;
        selected = i;
        sensor = cfg->remote_sensor[i];
        sc_remote_modbus_decode(&sensor, &settings);
        poll_cursor = (i + 1) % SC_MAX_REMOTE_SENSORS;
        break;
    }
    sc_config_release();
    if (selected == SC_MAX_REMOTE_SENSORS) return;
    taskENTER_CRITICAL();
    next_poll_us[selected] = now + (int64_t)settings.poll_sec * 1000000LL;
    taskEXIT_CRITICAL();
    float value;
    bool success = poll_remote(selected, &sensor, &settings, &value);
    cfg = sc_config_acquire();
    bool unchanged = cfg->remote_transport[selected] == SC_REMOTE_MODBUS_TCP &&
                     !memcmp(&cfg->remote_sensor[selected], &sensor, sizeof(sensor));
    if (success && unchanged) sc_automation_remote_value(selected, value);
    sc_config_release();
    if (!unchanged) sc_modbus_remote_changed(selected);
}

static int send_all(int socket_fd, const uint8_t *data, int length)
{
    int sent = 0;
    while (sent < length) {
        int result = send(socket_fd, data + sent, length - sent, 0);
        if (result <= 0) return -1;
        sent += result;
    }
    return 0;
}

static void exception(uint8_t *response, int *length, uint8_t function, uint8_t code)
{
    response[(*length)++] = function | 0x80;
    response[(*length)++] = code;
}

static int handle(int socket_fd, const uint8_t *request, int length)
{
    if (length < 8 || request[2] || request[3]) return -1;
    uint8_t unit = request[6], function = request[7];
    if (unit != modbus_unit_id) return 0;
    uint8_t response[64] = {request[0], request[1], 0, 0, 0, 0, unit};
    int response_length = 7;
    uint16_t relay_count = sc_relay_count();

    if (function == 1 && length >= 12) {
        uint16_t address = (request[8] << 8) | request[9];
        uint16_t quantity = (request[10] << 8) | request[11];
        if (quantity < 1 || quantity > SC_MAX_RELAYS) {
            exception(response, &response_length, function, 3);
        } else if ((uint32_t)address + quantity > relay_count) {
            exception(response, &response_length, function, 2);
        } else {
            response[response_length++] = function;
            response[response_length++] = (quantity + 7) / 8;
            for (int i = 0; i < quantity; i++)
                if (sc_relay_get(address + i)) response[response_length + i / 8] |= 1 << (i % 8);
            response_length += (quantity + 7) / 8;
        }
    } else if (function == 5 && length >= 12) {
        uint16_t address = (request[8] << 8) | request[9];
        uint16_t value = (request[10] << 8) | request[11];
        if (address >= relay_count || (value != 0xFF00 && value != 0)) {
            exception(response, &response_length, function, 2);
        } else {
            sc_relay_cmd_t cmd = {.relay = address, .op = SC_RELAY_SET, .value = value == 0xFF00};
            if (!sc_relay_submit(&cmd)) {
                exception(response, &response_length, function, 4);
            } else {
                memcpy(response + response_length, request + 7, 5);
                response_length += 5;
            }
        }
    } else if (function == 15 && length >= 13) {
        uint16_t address = (request[8] << 8) | request[9];
        uint16_t quantity = (request[10] << 8) | request[11];
        uint8_t byte_count = request[12];
        uint8_t required = (quantity + 7) / 8;
        if (quantity < 1 || quantity > SC_MAX_RELAYS || byte_count != required ||
            (uint32_t)address + quantity > relay_count || length < 13 + byte_count) {
            exception(response, &response_length, function, 2);
        } else {
            bool accepted = true;
            for (int i = 0; i < quantity; i++) {
                bool value = (request[13 + i / 8] >> (i % 8)) & 1;
                sc_relay_cmd_t cmd = {.relay = address + i, .op = SC_RELAY_SET, .value = value};
                if (!sc_relay_submit(&cmd)) accepted = false;
            }
            if (!accepted) {
                exception(response, &response_length, function, 4);
            } else {
                response[response_length++] = function;
                memcpy(response + response_length, request + 8, 4);
                response_length += 4;
            }
        }
    } else {
        exception(response, &response_length, function, 1);
    }

    uint16_t mbap_length = response_length - 6;
    response[4] = mbap_length >> 8;
    response[5] = mbap_length & 0xff;
    return send_all(socket_fd, response, response_length);
}

static int serve_client(int socket_fd)
{
    uint8_t buffer[MODBUS_ADU_MAX];
    int used = 0;
    int64_t pending_since_us = 0;
    for (;;) {
        int received = recv(socket_fd, buffer + used, sizeof(buffer) - used, 0);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const sc_config_t *cfg = sc_config_acquire();
            bool active = cfg->modbus_active;
            sc_config_release();
            if (!active) return 0;
            if (used) {
                if (esp_timer_get_time() - pending_since_us > 2000000LL)
                    return -1;
            } else {
                process_rule_write();
                poll_one_remote();
            }
            continue;
        }
        if (received <= 0) return -1;
        if (!used) pending_since_us = esp_timer_get_time();
        used += received;
        while (used >= 7) {
            uint16_t mbap_length = (buffer[4] << 8) | buffer[5];
            int frame_length = 6 + mbap_length;
            if (mbap_length < 2 || frame_length > MODBUS_ADU_MAX) return -1;
            if (used < frame_length) break;
            if (handle(socket_fd, buffer, frame_length) < 0) return -1;
            used -= frame_length;
            if (used) memmove(buffer, buffer + frame_length, used);
            if (!used) pending_since_us = 0;
            if (!used) process_rule_write();
            poll_one_remote();
        }
        if (used == MODBUS_ADU_MAX) return -1;
    }
}

static void server_task(void *arg)
{
    (void)arg;
    int listen_socket = -1;
    uint16_t bound_port = 0;
    int64_t next_bind_us = 0;
    for (;;) {
        const sc_config_t *cfg = sc_config_acquire();
        bool active = cfg->modbus_active;
        uint16_t port = cfg->modbus_port;
        modbus_unit_id = cfg->modbus_unit_id;
        sc_config_release();
        if ((!active || bound_port != port) && listen_socket >= 0) {
            close(listen_socket);
            listen_socket = -1;
        }
        if (active && listen_socket < 0 && esp_timer_get_time() >= next_bind_us) {
            listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (listen_socket >= 0) {
                int yes = 1;
                setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                struct sockaddr_in address = {
                    .sin_family = AF_INET,
                    .sin_port = htons(port),
                    .sin_addr.s_addr = htonl(INADDR_ANY)
                };
                if (bind(listen_socket, (struct sockaddr *)&address,
                         sizeof(address)) < 0 || listen(listen_socket, 1) < 0) {
                    close(listen_socket);
                    listen_socket = -1;
                    next_bind_us = esp_timer_get_time() + 5000000LL;
                } else {
                    bound_port = port;
                }
            } else {
                next_bind_us = esp_timer_get_time() + 5000000LL;
            }
        }
        if (listen_socket >= 0) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_socket, &readfds);
            struct timeval wait = {.tv_usec = 250000};
            int ready = select(listen_socket + 1, &readfds, NULL, NULL, &wait);
            if (ready > 0) {
                struct sockaddr_in client_address;
                socklen_t client_length = sizeof(client_address);
                int client_socket = accept(listen_socket, (struct sockaddr *)&client_address, &client_length);
                if (client_socket >= 0) {
                    int keepalive = 1, idle = 30, interval = 10, count = 3;
                    setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE,
                               &keepalive, sizeof(keepalive));
                    setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPIDLE,
                               &idle, sizeof(idle));
                    setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPINTVL,
                               &interval, sizeof(interval));
                    setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPCNT,
                               &count, sizeof(count));
                    struct timeval timeout = {.tv_usec = 250000};
                    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout));
                    timeout.tv_sec = 2;
                    timeout.tv_usec = 0;
                    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO,
                               &timeout, sizeof(timeout));
                    serve_client(client_socket);
                    close(client_socket);
                }
            } else if (ready < 0) {
                close(listen_socket);
                listen_socket = -1;
                next_bind_us = esp_timer_get_time() + 5000000LL;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        process_rule_write();
        poll_one_remote();
    }
}

esp_err_t sc_modbus_ensure_started(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    bool needed = cfg->modbus_active || has_remote_sensors(cfg) ||
                  has_rule_writes(cfg);
    sc_config_release();
    if (!needed) return ESP_OK;
    taskENTER_CRITICAL();
    bool start = !task_running;
    if (start) task_running = true;
    taskEXIT_CRITICAL();
    if (!start) return ESP_OK;
    if (xTaskCreate(server_task, "modbus", 2304, NULL, 3, NULL) == pdPASS)
        return ESP_OK;
    taskENTER_CRITICAL();
    task_running = false;
    taskEXIT_CRITICAL();
    return ESP_ERR_NO_MEM;
}

esp_err_t sc_modbus_init(void)
{
    sc_automation_set_modbus_sender(sc_modbus_rule_submit);
    return sc_modbus_ensure_started();
}
