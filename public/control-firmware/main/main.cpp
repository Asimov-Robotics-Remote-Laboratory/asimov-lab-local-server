#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "Arduino.h"
#include "arduino_sketch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_websocket_client.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_sleep.h"

#define UART UART_NUM_2
#define BUF_SIZE 512
#define RD_BUF_SIZE (BUF_SIZE)

#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 512

#define ROLE "DEVICE"

#define INITIAL_STATE 0
#define SERIAL_STARTED_STATE 1
#define WIFI_STARTED_STATE 2
#define WIFI_CONNECTED_STATE 3
#define DEFINED_IP_STATE 4
#define AUTHENTICATED_STATE 5
#define WEBSOCKET_CONNECTED_STATE 6

static QueueHandle_t uart0_queue;
char *token;
int state;
bool arduino_task_running = false;
bool serial_task_running = false;

TaskHandle_t arduinoTaskHandle;
TaskHandle_t readSerialHandle;

esp_websocket_client_handle_t websocket_client;

extern "C"{
	void app_main(void);
}

void change_state(int new_state);

void led_controll_bip(int times, int time){
	int count = 0;
	gpio_set_level(GPIO_NUM_2, 0); 				// @suppress("Invalid arguments")
	while(count < times){
		gpio_set_level(GPIO_NUM_2, 1); 			// @suppress("Invalid arguments")
		vTaskDelay(time / portTICK_PERIOD_MS); 	// @suppress("Invalid arguments")
		gpio_set_level(GPIO_NUM_2, 0); 			// @suppress("Invalid arguments")
		vTaskDelay(time / portTICK_PERIOD_MS); 	// @suppress("Invalid arguments")
		count++;
	}
}

void arduino_task(void* pvParamters){
	initArduino();
	setup();
	while(true){
		loop();
	}
}

char *outx;
cJSON *objx;

void serial_read_task(void * pvParameters)
{
	esp_websocket_client_handle_t client = (esp_websocket_client_handle_t) pvParameters;

	uart_event_t event;
	uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
	while (1) {
		if(xQueueReceive(uart0_queue, (void * )&event, (portTickType)portMAX_DELAY)) {
		    bzero(dtmp, RD_BUF_SIZE);

		    switch(event.type) {
		    	case UART_DATA:
					{
						const int rxBytes = uart_read_bytes(UART, dtmp, event.size, portMAX_DELAY); // @suppress("Invalid arguments")
						if (rxBytes > 0) {
							dtmp[rxBytes] = '\0';
							objx = cJSON_CreateObject();
							cJSON_AddItemToObject(objx, "messageType", cJSON_CreateString("RX_SERIAL"));
							cJSON_AddItemToObject(objx, "role", cJSON_CreateString(ROLE));
							cJSON_AddItemToObject(objx, "token", cJSON_CreateString(token));
							cJSON_AddItemToObject(objx, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
							cJSON_AddItemToObject(objx, "message", cJSON_CreateString((char*) dtmp));
							outx = cJSON_Print(objx);

							cJSON_Delete(objx);

							if(esp_websocket_client_is_connected(client)){
								esp_websocket_client_send(client, outx, strlen(outx), portMAX_DELAY); // @suppress("Invalid arguments")
								cJSON_free(outx);
							}
							uart_flush_input(UART);
							xQueueReset(uart0_queue);
						}
						break;
					}
		    	case UART_BUFFER_FULL:
		    		uart_flush_input(UART);
		    		xQueueReset(uart0_queue);
		    		break;
		    	case UART_FIFO_OVF:
					uart_flush_input(UART);
					xQueueReset(uart0_queue);
					break;
		    	case UART_BREAK:
					break;
				case UART_PARITY_ERR:
					break;
				case UART_FRAME_ERR:
					break;
				case UART_PATTERN_DET:
					break;
				default:
				    break;
		    }
		}
	}
	free(dtmp);
	dtmp = NULL;
}

void start_serial_read_task(){
	xTaskCreatePinnedToCore(serial_read_task, "serial_read_task", 4*1024, websocket_client, 10, &readSerialHandle, 0);
}

void start_arduino_task(){
	if(!arduino_task_running){
		led_controll_bip(2,100);
		arduino_task_running = true;
		xTaskCreatePinnedToCore(arduino_task, "arduino_task", 4*1024, NULL, 10, &arduinoTaskHandle, 1);
	}
}

void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	websocket_client = (esp_websocket_client_handle_t) handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;


    if(event_id == WEBSOCKET_EVENT_CONNECTED){
    	change_state(WEBSOCKET_CONNECTED_STATE);
    	cJSON *obj = cJSON_CreateObject();
		cJSON_AddItemToObject(obj, "messageType", cJSON_CreateString("JOIN_LABORATORY"));
		cJSON_AddItemToObject(obj, "role", cJSON_CreateString(ROLE));
		cJSON_AddItemToObject(obj, "token", cJSON_CreateString(token));
		cJSON_AddItemToObject(obj, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
		char *out = cJSON_Print(obj);
		cJSON_Delete(obj);
		esp_websocket_client_send_text(websocket_client, out, strlen(out), portMAX_DELAY); // @suppress("Invalid arguments")
		free(out);
    }

    if(event_id == WEBSOCKET_EVENT_DATA){
    	char msg[data->data_len+1]; // @suppress("Field cannot be resolved")
		strncpy(msg, &data->data_ptr[0], data->data_len); // @suppress("Invalid arguments") // @suppress("Field cannot be resolved")
		msg[data->data_len] = '\0'; // @suppress("Field cannot be resolved")

		cJSON *root = cJSON_Parse(msg);
		cJSON *obj;
		char *out;

		if(cJSON_GetObjectItem(root, "messageType") == NULL){
			return;
		}

		char *messageType = cJSON_GetObjectItem(root, "messageType")->valuestring;

		if(strcmp(messageType,"JOIN_LABORATORY_OK") == 0){
			start_serial_read_task();
			obj = cJSON_CreateObject();
			cJSON_AddItemToObject(obj, "messageType", cJSON_CreateString("DEVICE_STATE"));
			cJSON_AddItemToObject(obj, "role", cJSON_CreateString(ROLE));
			cJSON_AddItemToObject(obj, "token", cJSON_CreateString(token));
			cJSON_AddItemToObject(obj, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
			cJSON_AddItemToObject(obj, "message", cJSON_CreateString("ON_LINE"));
			out = cJSON_Print(obj);
			cJSON_Delete(obj);
			
			esp_websocket_client_send_text(websocket_client, out, strlen(out), portMAX_DELAY); // @suppress("Invalid arguments")
			
			free(out);
		}

		if(strcmp(messageType,"NEW_FIRMWARE") == 0){
			esp_http_client_config_t config = {};
			config.url = CONFIG_NEW_FIRMWARE_SERVICE_URL, // @suppress("Field cannot be resolved")
			config.cert_pem = NULL, // @suppress("Field cannot be resolved")

			obj = cJSON_CreateObject();
			cJSON_AddItemToObject(obj, "messageType", cJSON_CreateString("DEVICE_STATE"));
			cJSON_AddItemToObject(obj, "role", cJSON_CreateString(ROLE));
			cJSON_AddItemToObject(obj, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
			cJSON_AddItemToObject(obj, "token", cJSON_CreateString(token));
			cJSON_AddItemToObject(obj, "message", cJSON_CreateString("RESTART"));
			out = cJSON_Print(obj);
			cJSON_Delete(obj);

			esp_websocket_client_send_text(websocket_client, out, strlen(out), portMAX_DELAY); // @suppress("Invalid arguments")
			
			free(out);
			
			esp_https_ota(&config);
			esp_deep_sleep_start();
		}

		if(strcmp(messageType,"TX_SERIAL") == 0){
			const char *message = cJSON_GetObjectItem(root,"message")->valuestring;
			int size = strlen(message)+1;
			char copy[size];
			strcpy(copy, message);
			copy[size-1] = '\n';
			uart_write_bytes(UART, (const char*) copy, size);
			uart_flush(UART);
		}

		if(strcmp(messageType,"JOYSTICK_COMM") == 0){
			const char *message = cJSON_GetObjectItem(root,"message")->valuestring;
			int size = strlen(message)+1;
			char copy[size];
			strcpy(copy, message);
			copy[size-1] = '\n';
			uart_write_bytes(UART, (const char*) copy, size);
			uart_flush(UART);
		}

		if(strcmp(messageType,"START_ARDUINO_TASK") == 0){
			start_arduino_task();

			obj = cJSON_CreateObject();
			cJSON_AddItemToObject(obj, "messageType", cJSON_CreateString("START_ARDUINO_TASK_OK"));
			cJSON_AddItemToObject(obj, "role", cJSON_CreateString(ROLE));
			cJSON_AddItemToObject(obj, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
			cJSON_AddItemToObject(obj, "token", cJSON_CreateString(token));
			out = cJSON_Print(obj);
			cJSON_Delete(obj);
			
			esp_websocket_client_send_text(websocket_client, out, strlen(out), portMAX_DELAY); // @suppress("Invalid arguments")
			
			free(out);
		}

		if(strcmp(messageType,"RESTART_DEVICE") == 0){
			obj = cJSON_CreateObject();
			cJSON_AddItemToObject(obj, "messageType", cJSON_CreateString("DEVICE_STATE"));
			cJSON_AddItemToObject(obj, "role", cJSON_CreateString(ROLE));
			cJSON_AddItemToObject(obj, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
			cJSON_AddItemToObject(obj, "token", cJSON_CreateString(token));
			cJSON_AddItemToObject(obj, "message", cJSON_CreateString("RESTART"));
			out = cJSON_Print(obj);
			cJSON_Delete(obj);

			esp_websocket_client_send_text(websocket_client, out, strlen(out), portMAX_DELAY); // @suppress("Invalid arguments")
			
			free(out);
			
			led_controll_bip(4,100);
			esp_deep_sleep_start();
		}

		if(strcmp(messageType,"ERROR_MESSAGE") == 0){
			esp_deep_sleep_start();
		}

		cJSON_Delete(root);
    }

    if(event_id == WEBSOCKET_EVENT_ERROR){
    	esp_deep_sleep_start();
    }

    if(event_id == WEBSOCKET_EVENT_DISCONNECTED){
       	esp_deep_sleep_start();
    }
}

void websocket_init(void)
{
	esp_websocket_client_config_t websocket_cfg = { // @suppress("Invalid arguments")
			.buffer_size=256
	};
	websocket_cfg.uri = CONFIG_WS_URL; // @suppress("Field cannot be resolved")
	esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
	esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client); // @suppress("Invalid arguments")
	esp_websocket_client_start(client);
}

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
	static char *output_buffer;  // Buffer to store response of http request from event handler
	static int output_len;       // Stores number of bytes read

	switch(evt->event_id) {
		case HTTP_EVENT_ON_CONNECTED:
			break;
		case HTTP_EVENT_HEADERS_SENT:
			break;
		case HTTP_EVENT_ON_HEADER:
			break;
		case HTTP_EVENT_ON_FINISH:
			break;
		case HTTP_EVENT_DISCONNECTED:
			break;
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
				if (evt->user_data) {
					memcpy(evt->user_data + output_len, evt->data, evt->data_len);
				} else {
					if (output_buffer == NULL) {
						output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
						output_len = 0;
						if (output_buffer == NULL) {
							return ESP_FAIL;
						}
					}
					memcpy(output_buffer + output_len, evt->data, evt->data_len);
				}
				output_len += evt->data_len;
            }

			break;
        case HTTP_EVENT_ERROR:
        	esp_deep_sleep_start();
        	break;
    }
    return ESP_OK;
}

void authenticate()
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddItemToObject(obj, "role", cJSON_CreateString(ROLE));
	cJSON_AddItemToObject(obj, "laboratoryId", cJSON_CreateString(CONFIG_LABORATORY_ID));
	cJSON_AddItemToObject(obj, "password", cJSON_CreateString(CONFIG_LABORATORY_PASSWORD));
	const char *post_data = cJSON_Print(obj);
	cJSON_Delete(obj);

	char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

	esp_http_client_config_t config = { // @suppress("Invalid arguments")
		.url = CONFIG_AUTHENTICATION_SERVICE_URL,
		.event_handler = http_event_handler,
		.user_data = local_response_buffer
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);
	esp_http_client_set_method(client, HTTP_METHOD_POST); // @suppress("Invalid arguments")
	esp_http_client_set_header(client, "Accept", "application/json");
	esp_http_client_set_header(client, "Content-Type", "application/json");
	esp_http_client_set_post_field(client, post_data, strlen(post_data));

	esp_err_t err = esp_http_client_perform(client);

	if (err == ESP_OK) {
		cJSON *root = cJSON_Parse(local_response_buffer);
		token = cJSON_GetObjectItem(root,"token")->valuestring;
		change_state(AUTHENTICATED_STATE);
	}else {
		esp_deep_sleep_start();
	}
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *dados_do_evento)
{
	//Evento realizado quando a wifi for configurada e inicializada
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		change_state(WIFI_STARTED_STATE);
	}

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		change_state(WIFI_CONNECTED_STATE);
	}

	//Evento realizado quando o modulo receber um n�mero IP do DHCP do roteador
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		change_state(DEFINED_IP_STATE);
	}

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		esp_deep_sleep_start();
	}

}

void serial_init()
{
	const uart_port_t uart_num = UART;

	uart_config_t uart_config = { // @suppress("Invalid arguments")
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.rx_flow_ctrl_thresh = UART_SCLK_APB,
	};

	uart_driver_install(UART, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
	uart_param_config(UART, &uart_config);
	uart_set_pin(UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	uart_pattern_queue_reset(UART, 20);
	uart_param_config(uart_num, &uart_config);

	change_state(SERIAL_STARTED_STATE);
}

void wifi_init()
{
	esp_netif_create_default_wifi_sta();
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL); // @suppress("Invalid arguments")
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL); // @suppress("Invalid arguments")

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // @suppress("Invalid arguments") // @suppress("Symbol is not resolved")
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // @suppress("Invalid arguments")
    esp_wifi_set_mode(WIFI_MODE_STA);// @suppress("Invalid arguments")

    wifi_config_t sta_config = {};
    strcpy((char*)sta_config.sta.ssid,  CONFIG_WIFI_SSID); // @suppress("Field cannot be resolved")
    strcpy((char*)sta_config.sta.password, CONFIG_WIFI_PASSWORD); // @suppress("Field cannot be resolved")

    esp_wifi_set_config(WIFI_IF_STA, &sta_config); // @suppress("Invalid arguments")
	esp_wifi_start();
}

void change_state(int new_state)
{
	state = new_state;
	switch(state) {
		case INITIAL_STATE:
			serial_init();
			break;
		case SERIAL_STARTED_STATE:
			wifi_init();
			break;
		case WIFI_STARTED_STATE:
			esp_wifi_connect();
			break;
		case WIFI_CONNECTED_STATE:
			break;
		case DEFINED_IP_STATE:
			authenticate();
			break;
		case AUTHENTICATED_STATE:
			websocket_init();
			break;
		case WEBSOCKET_CONNECTED_STATE:
			led_controll_bip(1, 1000);
			break;
	}
}

void shutdown_handler(){

}

void app_main(void)
{
	nvs_flash_init();
	esp_netif_init();
	esp_event_loop_create_default();
	esp_sleep_enable_timer_wakeup(500);
	gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT); // @suppress("Invalid arguments")
	change_state(INITIAL_STATE);
}

