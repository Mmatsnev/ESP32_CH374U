#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_wifi.h"

#include "usb_hub.h"
#include "esp_bluetooth.h"
#include "adb_device.h"

#include "sha1withrsa.h"
#include "led_hal.h"
#include "upgrade.h"
#include "version.h"
#include "esp_wifi_station.h"

#include "nvs_flash.h"
#include "nvs.h"

const uint8_t VERSION[3] = {VERSION_MASTER,VERSION_RELEASE,VERSION_DEBUG};

void app_main()
{

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    led_init();

    printf("==============================================\r\n");
    printf("===   version:%d.%d.%d\r\n",VERSION[0],VERSION[1],VERSION[2]);
    printf("==============================================\r\n");

    if(KEY_READ() == 0x00)
    {
        upgrade_init();

        LED_USB0_LOW();
        LED_USB1_LOW();
        LED_USB2_LOW();

        while(1) 
        {
            LED_STATUS_LOW();
            vTaskDelay(20 / portTICK_RATE_MS);
            LED_STATUS_HIGH();
            vTaskDelay(480 / portTICK_RATE_MS);
        }        
    }
    else
    {
        wifi_init_station();
        //esp_bluetooth_init(); 不使用蓝牙功能
        xTaskCreate(usb_hub_task, "usb_hub_task", 16*1024, NULL, 6, NULL);

        while(1) 
        {
            led_status_turn();
            vTaskDelay(1000 / portTICK_RATE_MS);
        }        
    }
}

