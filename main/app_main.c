#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "apps/sntp/sntp.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "ping.h"
#include "esp_ping.h"

#include "driver/gpio.h"

#include "config.h"  // SSIDs, PSKs, URIs ...

#define BLINK_GPIO 2  // Pin 2 builtin LED

static const char *TAG = "LED_ESP32";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;
static char ssid[33];
static char esp_ip[16];
static char boot_time[64];
static int online = 0;
static int mqtt_connected = 0;
esp_mqtt_client_handle_t client;
TaskHandle_t wifi_init_task_handle = NULL;



void blink_task(void *pvParameter)
{
    gpio_pad_select_gpio(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    while(1) {
        /* Blink off (output low) */
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay((online ? 1950 : 150) / portTICK_PERIOD_MS);
        /* Blink on (output high) */
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}


esp_err_t pingResults(ping_target_id_t msgType, esp_ping_found * pf){
    //printf("AvgTime:%.1fmS Sent:%d Rec:%d Err:%d min(mS):%d max(mS):%d\n", (float)pf->total_time/pf->recv_count, 
    //   pf->send_count, pf->recv_count, pf->err_count, pf->min_time, pf->max_time );
    if (pf->send_count == pf->recv_count) online = 1;
    else online = 0;
    return ESP_OK;
}

void pingcheck_task(void *pvParameter)
{
    ip_addr_t ip_Addr;
    IP_ADDR4( &ip_Addr, 8, 8, 8, 8);
    uint32_t ping_count = 1;  //how many pings per report
    uint32_t ping_timeout = 3000; //mS till we consider it timed out
    uint32_t ping_delay = 500; //mS between pings

    esp_ping_set_target(PING_TARGET_IP_ADDRESS_COUNT, &ping_count, sizeof(uint32_t));
    esp_ping_set_target(PING_TARGET_RCV_TIMEO, &ping_timeout, sizeof(uint32_t));
    esp_ping_set_target(PING_TARGET_DELAY_TIME, &ping_delay, sizeof(uint32_t));
    esp_ping_set_target(PING_TARGET_IP_ADDRESS, &ip_Addr, sizeof(uint32_t));
    esp_ping_set_target(PING_TARGET_RES_FN, &pingResults, sizeof(pingResults));

    while (1) {
        //printf("Pinging IP %s\n", inet_ntoa(ip_Addr));
        ping_init();
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void publish_task(void *pvParameter) 
{   
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64]; 
    char msg[200];

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);
				if (timeinfo.tm_year < (2016 - 1900)) {
					sntp_init();
				}
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

        strcpy(msg, "{\"command\": \"udevice\", \"idx\": 138, \"svalue\": \"");
        strcat(msg, strftime_buf);
        strcat(msg, ", SSID: ");
        strcat(msg, ssid);
        strcat(msg, ", IP: ");
        strcat(msg, esp_ip);
        strcat(msg, ", Boot: ");
        strcat(msg, boot_time);
        strcat(msg, "\"}");
				if (mqtt_connected) {
					printf("Publish Data: %s\n", msg);
					esp_mqtt_client_publish(client, "domoticz/in", msg, sizeof(msg), 0, 0);
				} else {
					printf("mqtt disconnected\n");
				}
				esp_mqtt_client_publish(client, "stat/esp32/online", "1", 1, 0, 0);
				
        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            //printf("STA_START\n");
            //esp_wifi_connect();
            break;
        case SYSTEM_EVENT_SCAN_DONE:
            //printf("SCAN_DONE\n");
            break;
        case SYSTEM_EVENT_STA_CONNECTED:
            //printf("STA_CONNECTED\n");
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
            strcpy(esp_ip, ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            break;
        case SYSTEM_EVENT_STA_LOST_IP:
				    printf("STA_LOST_IP");
				    online = 0;
						break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        case SYSTEM_EVENT_AP_START:
            //printf("AP_START");
            break;
        case SYSTEM_EVENT_AP_STOP:
            //printf("AP_STOP");
            break;
        default:
            printf("wifi_event: %d\n", event->event_id);
            break;
    }
    return ESP_OK;
}

//static void wifi_init(void)
void wifi_init_task(void *pvParameter) 
{
    //tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_scan_start(NULL, true);
    
    uint16_t ap_num;
    int ssid_found = 0;
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD
        },
    };

    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t ap_records[ap_num];
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);

    printf("%d SSIDs found:\n", ap_num);
    printf(" Channel | RSSI | SSID \n");
    for (int i = 0; i < ap_num; i++) {
        printf("%8d |%5d | %-27s \n", ap_records[i].primary, ap_records[i].rssi, (char *)ap_records[i].ssid);
        if (strcmp((char *)ap_records[i].ssid, CONFIG_WIFI_SSID) == 0) {
            strcpy((char *)wifi_config.sta.password, (const char*)CONFIG_WIFI_PASSWORD);
            ssid_found = 1;
        }
        if (strcmp((char *)ap_records[i].ssid, WIFI_SSID1) == 0) {
            strcpy((char *)wifi_config.sta.password, (const char*)WIFI_PSK1);
            ssid_found = 1;
        }
        if (strcmp((char *)ap_records[i].ssid, WIFI_SSID2) == 0) {
            strcpy((char *)wifi_config.sta.password, (const char*)WIFI_PSK2);
            ssid_found = 1;
	}
        if (strcmp((char *)ap_records[i].ssid, WIFI_SSID3) == 0) {
            strcpy((char *)wifi_config.sta.password, (const char*)WIFI_PSK3);
            ssid_found = 1;
	}
        if (ssid_found == 1) {
            strcpy(ssid, (const char *)ap_records[i].ssid);
            strcpy((char *)wifi_config.sta.ssid, (const char *)ap_records[i].ssid);
            printf("connecting to: %s\n", ssid);
            break;
	}
    }
    //ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s] password:[%s]", CONFIG_WIFI_SSID, "******");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    ESP_ERROR_CHECK(esp_wifi_connect());
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
		vTaskDelete(wifi_init_task_handle);
}

extern const uint8_t iot_eclipse_org_pem_start[] asm("_binary_iot_eclipse_org_pem_start");
extern const uint8_t iot_eclipse_org_pem_end[]   asm("_binary_iot_eclipse_org_pem_end");

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = 1;
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "cmnd/esp32/restart", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "stat/esp32/online", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "cmnd/esp32/qos1", 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_unsubscribe(client, "cmnd/esp32/qos1");
            ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = 0;
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            //msg_id = esp_mqtt_client_publish(client, "stat/esp32/online", "1", 0, 0, 0);
            //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            //printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            //printf("DATA=%.*s\r\n", event->data_len, event->data);
						if (strncmp("stat/esp32/online", event->topic, event->topic_len) == 0) {
							printf("hearbeat received\n");
							mqtt_connected = 1;
						}
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
    }
    return ESP_OK;
}

static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = MQTT_URI,
        .event_handle = mqtt_event_handler,
        //.cert_pem = (const char *)iot_eclipse_org_pem_start,
        //.host = MQTT_HOST,
        //.port = MQTT_PORT,
        //.client_id = MQTT_CLIENT_ID,
        .username = MQTT_USER,
        .password = MQTT_PASSWORD
    };

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
    return client;
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void obtain_time(void)
{
    initialize_sntp();

    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

void time_init()
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
        obtain_time();
        // update 'now' variable with current time
        time(&now);
    }

    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
    printf("Time: %s\n", strftime_buf);
    strcpy(boot_time, strftime_buf);
}

void app_main()
{
    TaskHandle_t publish_task_handle = NULL; //, wifi_init_task_handle = NULL;

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_NONE);
    /*    
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);
    */

    nvs_flash_init();
    printf("starting blink task\n");
    xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE, NULL, 5, NULL);

		tcpip_adapter_init();
    printf("starting pingcheck task\n");
    xTaskCreate(&pingcheck_task, "pingcheck_task", configMINIMAL_STACK_SIZE, NULL, 5, NULL);
    
		xTaskCreate(&wifi_init_task, "wifi_init_task", 2048, NULL, 5, &wifi_init_task_handle);
    //wifi_init();
		
		int timeout = 0;
		while (!online && timeout++ < 60) {
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
		if (wifi_init_task_handle == NULL) {
		  vTaskDelete(wifi_init_task_handle);
		}
		
    time_init();

    while (1) {
        if (online) {
            //printf("ONLINE\n");
            if (publish_task_handle == NULL) {
                printf("starting publish task\n");
                client = mqtt_app_start();
                //vTaskDelay(1000 / portTICK_PERIOD_MS);
                xTaskCreate(&publish_task, "publish_task", 2048, NULL, 5, &publish_task_handle);
            }
        } else { // offline
            printf("!!! OFFLINE !!!\n");
            if (publish_task_handle != NULL) {
                printf("deleting publish task\n");
                vTaskDelete(publish_task_handle);
								publish_task_handle = NULL;
            }
        }
				
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
