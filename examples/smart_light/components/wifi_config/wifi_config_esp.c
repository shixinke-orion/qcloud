/*
 * Copyright (c) 2020 Tencent Cloud. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "esp_event.h"

#include "esp_netif.h"

#include "qcloud_iot_export.h"
#include "qcloud_iot_import.h"
#include "utils_hmac.h"
#include "utils_base64.h"

#include "wifi_config_internal.h"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t sg_wifi_event_group;
static const int          CONNECTED_BIT          = BIT0;
static const int          ESPTOUCH_DONE_BIT      = BIT1;
static const int          STA_DISCONNECTED_BIT = BIT2;

static bool sg_wifi_init_done     = false;
static bool sg_wifi_sta_connected = false;

//============================ ESP wifi functions begin ===========================//

bool is_wifi_sta_connected(void)
{
    return sg_wifi_sta_connected;
}

static void _wifi_event_handler_new(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    Log_i("event id: %d", event_id);
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            Log_i("WIFI_EVENT_STA_START");
            break;

        case WIFI_EVENT_STA_CONNECTED:{
            wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t*)event_data;
             Log_i("SYSTEM_EVENT_STA_CONNECTED to AP %s at channel %u", (char *)event->ssid,
                   event->channel);
            // PUSH_LOG("STA_CONNECTED to AP %s at ch %u", (char *)event->event_info.connected.ssid,
            //          event->event_info.connected.channel);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED:{
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t*)event_data;
             Log_e("SYSTEM_EVENT_STA_DISCONNECTED from AP %s reason: %u", (char *)event->ssid,
                   event->reason);
            // PUSH_LOG("STA_DISCONNECTED from AP %s reason: %u", (char *)event->event_info.disconnected.ssid,
            //          event->event_info.disconnected.reason);
            xEventGroupClearBits(sg_wifi_event_group, CONNECTED_BIT);
            xEventGroupSetBits(sg_wifi_event_group, STA_DISCONNECTED_BIT);
            sg_wifi_sta_connected = false;
            // push_error_log(ERR_WIFI_CONNECT, event->event_info.disconnected.reason);
            break;
        }

        case WIFI_EVENT_AP_START: {
            uint8_t            channel = 0;
            wifi_second_chan_t second;
            esp_wifi_get_channel(&channel, &second);
            Log_i("SYSTEM_EVENT_AP_START at channel %u", channel);
            break;
        }

        case WIFI_EVENT_AP_STOP:
            Log_i("SYSTEM_EVENT_AP_STOP");
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t*)event_data;
             Log_i("SYSTEM_EVENT_AP_STACONNECTED, mac:" MACSTR ", aid:%d", MAC2STR(event->mac),
                   event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t*)event_data;
            // xEventGroupSetBits(sg_wifi_event_group, STA_DISCONNECTED_BIT);
            Log_i("SYSTEM_EVENT_AP_STADISCONNECTED, mac:" MACSTR ", aid:%d", MAC2STR(event->mac),
                  event->aid);
            break;
        }

        case SYSTEM_EVENT_AP_STAIPASSIGNED:
            Log_i("SYSTEM_EVENT_AP_STAIPASSIGNED");
            break;

        default:
            Log_i("unknown event id: %d", event_id);
            break;
    }

}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    Log_i("Got IPv4[%s]", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
    xEventGroupSetBits(sg_wifi_event_group, CONNECTED_BIT);
}

int wifi_ap_init(const char *ssid, const char *psw, uint8_t ch)
{
    esp_err_t rc;
    if (!sg_wifi_init_done) {
        esp_netif_init();
        sg_wifi_event_group = xEventGroupCreate();
        if (sg_wifi_event_group == NULL) {
            Log_e("xEventGroupCreate failed!");
            return ESP_ERR_NO_MEM;
        }
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        rc                     = esp_wifi_init(&cfg);
        if (rc != ESP_OK) {
            Log_e("esp_wifi_init failed: %d", rc);
            return rc;
        }
        sg_wifi_init_done = true;
    }

    sg_wifi_sta_connected = false;

    xEventGroupClearBits(sg_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT | STA_DISCONNECTED_BIT);

    // should disconnect first, could be failed if not connected
    rc = esp_wifi_disconnect();
    if (ESP_OK != rc) {
        Log_w("esp_wifi_disconnect failed: %d", rc);
    }

    rc = esp_wifi_stop();
    if (rc != ESP_OK) {
        Log_w("esp_wifi_stop failed: %d", rc);
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler_new, NULL));

    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_storage failed: %d", rc);
        return rc;
    }

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.ap.ssid, ssid);
    wifi_config.ap.ssid_len       = strlen(ssid);
    wifi_config.ap.max_connection = 3;
    wifi_config.ap.channel        = (uint8_t)ch;
    if (psw) {
        strcpy((char *)wifi_config.ap.password, psw);
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    rc = esp_wifi_set_mode(WIFI_MODE_AP);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_mode failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_config failed: %d", rc);
        return rc;
    }

    return ESP_OK;
}

// connect in APSTA mode
int wifi_ap_sta_connect(const char *ssid, const char *psw)
{
    static wifi_config_t router_wifi_config = {0};
    memset(&router_wifi_config, 0, sizeof(router_wifi_config));
    strncpy((char *)router_wifi_config.sta.ssid, ssid, 32);
    strncpy((char *)router_wifi_config.sta.password, psw, 64);

    esp_err_t rc = ESP_OK;

    rc = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_storage failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_mode failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_set_config(ESP_IF_WIFI_STA, &router_wifi_config);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_config failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_connect();
    if (ESP_OK != rc) {
        Log_e("esp_wifi_connect failed: %d", rc);
        return rc;
    }

    return 0;
}

// connect in STA mode
int wifi_sta_connect(const char *ssid, const char *psw)
{
    static wifi_config_t router_wifi_config = {0};
    memset(&router_wifi_config, 0, sizeof(router_wifi_config));
    strncpy((char *)router_wifi_config.sta.ssid, ssid, 32);
    strncpy((char *)router_wifi_config.sta.password, psw, 64);

    esp_err_t rc = ESP_OK;

    rc = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_storage failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_mode failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_set_config(ESP_IF_WIFI_STA, &router_wifi_config);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_config failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_connect();
    if (ESP_OK != rc) {
        Log_e("esp_wifi_connect failed: %d", rc);
        return rc;
    }

    return 0;
}

int wifi_stop_softap(void)
{
    Log_i("Switch to STA mode");

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        Log_e("esp_wifi_set_mode STA failed");
    }

    return 0;
}

// static void _smartconfig_event_cb(smartconfig_status_t status, void *pdata)
// {
//     switch (status) {
//         case SC_STATUS_WAIT:
//             Log_i("SC_STATUS_WAIT");
//             break;
//         case SC_STATUS_FIND_CHANNEL:
//             Log_i("SC_STATUS_FINDING_CHANNEL");
//             break;
//         case SC_STATUS_GETTING_SSID_PSWD:
//             Log_i("SC_STATUS_GETTING_SSID_PSWD");
//             break;
//         case SC_STATUS_LINK:
//             if (pdata) {
//                 wifi_config_t *wifi_config = pdata;
//                 Log_i("SC_STATUS_LINK SSID:%s PSW:%s", wifi_config->sta.ssid, wifi_config->sta.password);
//                 PUSH_LOG("SC SSID:%s PSW:%s", wifi_config->sta.ssid, wifi_config->sta.password);

//                 int ret = esp_wifi_disconnect();
//                 if (ESP_OK != ret) {
//                     Log_e("esp_wifi_disconnect failed: %d", ret);
//                     // push_error_log(ERR_WIFI_DISCONNECT, ret);
//                 }

//                 ret = esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_config);
//                 if (ESP_OK != ret) {
//                     Log_e("esp_wifi_set_config failed: %d", ret);
//                     // push_error_log(ERR_WIFI_CONFIG, ret);
//                 }

//                 ret = esp_wifi_connect();
//                 if (ESP_OK != ret) {
//                     Log_e("esp_wifi_connect failed: %d", ret);
//                     push_error_log(ERR_WIFI_CONNECT, ret);
//                 }
//             } else {
//                 Log_e("invalid smart config link data");
//                 push_error_log(ERR_SC_DATA, ERR_SC_INVALID_DATA);
//             }
//             break;
//         case SC_STATUS_LINK_OVER:
//             Log_w("SC_STATUS_LINK_OVER");
//             if (pdata != NULL) {
//                 uint8_t phone_ip[4] = {0};
//                 memcpy(phone_ip, (uint8_t *)pdata, 4);
//                 Log_i("Phone ip: %d.%d.%d.%d\n", phone_ip[0], phone_ip[1], phone_ip[2], phone_ip[3]);
//             }

//             xEventGroupSetBits(sg_wifi_event_group, ESPTOUCH_DONE_BIT);
//             break;
//         default:
//             break;
//     }
// }

int wifi_sta_init(void)
{
    esp_err_t rc;
    if (!sg_wifi_init_done) {
        esp_netif_init();
        sg_wifi_event_group = xEventGroupCreate();
        if (sg_wifi_event_group == NULL) {
            Log_e("xEventGroupCreate failed!");
            return ESP_ERR_NO_MEM;
        }
         ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        rc                     = esp_wifi_init(&cfg);
        if (rc != ESP_OK) {
            Log_e("esp_wifi_init failed: %d", rc);
            return rc;
        }
        sg_wifi_init_done = true;

    }

    sg_wifi_sta_connected = false;

    xEventGroupClearBits(sg_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT | STA_DISCONNECTED_BIT);

    rc = esp_wifi_stop();
    if (rc != ESP_OK) {
        Log_w("esp_wifi_stop failed: %d", rc);
    }

    // if (esp_event_loop_init(_wifi_event_handler, NULL) && g_cb_bck == NULL) {
    //     Log_w("replace esp wifi event handler");
    //     g_cb_bck = esp_event_loop_set_cb(_wifi_event_handler, NULL);
    // }

    rc = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_storage failed: %d", rc);
        return rc;
    }

    rc = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rc != ESP_OK) {
        Log_e("esp_wifi_set_mode failed: %d", rc);
        return rc;
    }

    return ESP_OK;
}

int wifi_start_smartconfig(void)
{
    int ret = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS);
    if (ESP_OK != ret) {
        Log_e("esp_smartconfig_set_type failed: %d", ret);
        return ret;
    }

    // ret = esp_smartconfig_start(_smartconfig_event_cb);
    // if (ESP_OK != ret) {
    //     Log_e("esp_smartconfig_start failed: %d", ret);
    //     return ret;
    // }

    return ESP_OK;
}

int wifi_stop_smartconfig(void)
{
    int ret = esp_smartconfig_stop();
    xEventGroupClearBits(sg_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT | STA_DISCONNECTED_BIT);

    return ret;
}

int wifi_wait_event(unsigned int timeout_ms)
{
    EventBits_t uxBits =
        xEventGroupWaitBits(sg_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT | STA_DISCONNECTED_BIT, true, false,
                                             timeout_ms / portTICK_RATE_MS);
    if (uxBits & CONNECTED_BIT) {
        return EVENT_WIFI_CONNECTED;
    }

    if (uxBits & STA_DISCONNECTED_BIT) {
        return EVENT_WIFI_DISCONNECTED;
    }

    if (uxBits & ESPTOUCH_DONE_BIT) {
        return EVENT_SMARTCONFIG_STOP;
    }

    return EVENT_WAIT_TIMEOUT;
}

int wifi_start_running(void)
{
    return esp_wifi_start();
}
//============================ ESP wifi functions end ===========================//
