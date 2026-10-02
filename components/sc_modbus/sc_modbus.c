#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sc_config.h"
#include "sc_relay.h"
#include "sc_modbus.h"

#define MODBUS_ADU_MAX 128

static uint8_t modbus_unit_id = 1;

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
    for (;;) {
        int received = recv(socket_fd, buffer + used, sizeof(buffer) - used, 0);
        if (received <= 0) return -1;
        used += received;
        while (used >= 7) {
            uint16_t mbap_length = (buffer[4] << 8) | buffer[5];
            int frame_length = 6 + mbap_length;
            if (mbap_length < 2 || frame_length > MODBUS_ADU_MAX) return -1;
            if (used < frame_length) break;
            if (handle(socket_fd, buffer, frame_length) < 0) return -1;
            used -= frame_length;
            if (used) memmove(buffer, buffer + frame_length, used);
        }
        if (used == MODBUS_ADU_MAX) return -1;
    }
}

static void server_task(void *arg)
{
    (void)arg;
    for (;;) {
        const sc_config_t *cfg = sc_config_acquire();
        bool active = cfg->modbus_active;
        uint16_t port = cfg->modbus_port;
        sc_config_release();
        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int yes = 1;
        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = htonl(INADDR_ANY)
        };
        if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            listen(listen_socket, 1) < 0) {
            close(listen_socket);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        for (;;) {
            struct sockaddr_in client_address;
            socklen_t client_length = sizeof(client_address);
            int client_socket = accept(listen_socket, (struct sockaddr *)&client_address, &client_length);
            if (client_socket < 0) break;
            int keepalive = 1;
            int keepidle = 30;
            int keepinterval = 10;
            int keepcount = 3;
            struct timeval send_timeout = {.tv_sec = 3};
            setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
            setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
            setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPINTVL, &keepinterval, sizeof(keepinterval));
            setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount));
            setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
            serve_client(client_socket);
            close(client_socket);

            cfg = sc_config_acquire();
            active = cfg->modbus_active;
            sc_config_release();
            if (!active) break;
        }
        close(listen_socket);
    }
}

esp_err_t sc_modbus_init(void)
{
    const sc_config_t *cfg = sc_config_acquire();
    bool active = cfg->modbus_active;
    modbus_unit_id = cfg->modbus_unit_id;
    sc_config_release();
    if (!active) return ESP_OK;
    return xTaskCreate(server_task, "modbus", 2304, NULL, 3, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
