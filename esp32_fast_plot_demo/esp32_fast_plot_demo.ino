#include <Arduino.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <driver/i2s.h>
#include <driver/adc.h>
#include "esp_task_wdt.h"


//SSID and Password of your WiFi router
#include "my_wifi.h"  
///// my_wifi.h contains the following lines with my own wifi info.
///// You can remove the line above and uncomment the lines below with your info,
///// or make your own my_wifi.h containing the following lines with your info.
///// I had to do it this way in order to safely upload to github.
/*
const char* ssid = "my_wifi_ssid";
const char* password = "my_wifi_pw";
*/


/** ADC CONFIG **/
i2s_port_t i2s_port = I2S_NUM_0;   // I2S Port 
adc1_channel_t adc_channel = ADC1_CHANNEL_5; //GPIO33  (SET THIS FOR YOUR HARDWARE)
const uint16_t adc_sample_freq = 44100;
const uint16_t dma_buffer_len = 1024;
const uint16_t i2s_buffer_len = dma_buffer_len;
const uint16_t ws_tx_buffer_len = dma_buffer_len;
uint16_t* i2s_read_buff = (uint16_t*)calloc(i2s_buffer_len, sizeof(uint16_t));
uint16_t* ws_send_buffer = (uint16_t*)calloc(ws_tx_buffer_len, sizeof(uint16_t));


AsyncWebServer    server(80);
WebSocketsServer  webSocket = WebSocketsServer(81);
AsyncEventSource  events("/events");
AsyncWebServerRequest *request;
AsyncWebSocket    ws("/test");
AsyncWebSocketClient * globalClient = NULL;
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len);


boolean streaming = true;
boolean sampling  = true;
unsigned long prev_micros;
size_t bytes_read;
TaskHandle_t samplingTaskHandle;


// Prototypes:
void configure_i2s();
static void getDataLoopTask(void * pvParameters);
static const inline void sendSamples();
static const inline void getSamples();



//////////////////////////   CONFIGURE I2S SAMPLING   ////////////////////////////////
void configure_i2s(){
  i2s_config_t i2s_config = 
    {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),  // I2S receive mode with ADC
    .sample_rate = adc_sample_freq,                                               // set I2S ADC sample rate
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,                                 // 16 bit I2S (even though ADC is 12 bit)
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,                                 // handle adc data as single channel (right)
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_I2S,               // I2S format
    .intr_alloc_flags = 0,                                                        // 
    .dma_buf_count = 4,                                                           // number of DMA buffers >=2 for fastness
    .dma_buf_len = dma_buffer_len,                                                // number of samples per buffer
    .use_apll = 0,                                                                // no Audio PLL - buggy and not well documented
  };
  adc1_config_channel_atten(adc_channel, ADC_ATTEN_11db);
  adc1_config_width(ADC_WIDTH_12Bit);
  i2s_driver_install(i2s_port, &i2s_config, 0, NULL);
 
  i2s_set_adc_mode(ADC_UNIT_1, adc_channel);
  i2s_adc_enable(i2s_port);
  vTaskDelay(1000);
}


/////////////////////////   SERVER FUNCTIONS   /////////////////////////////////////
void serverons(){

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    while(!SPIFFS.exists("/index.html")){
      Serial.println("Error, /index.html is not onboard");
      delay(1000);
    }
    Serial.println("trying to send index.html");  // troubleshooting core crash if file not onboard
    AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/index.html", "text/html");
    response->addHeader("Access-Control-Max-Age", "10000");
    response->addHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");
    request->send(response);
    Serial.println("sent");
  });

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Sending /favicon.ico");
    request->send(SPIFFS, "/favicon.ico","text/css");
  });

  server.on("/jqueryjs", HTTP_GET, [](AsyncWebServerRequest* request) {
    Serial.println("sending /jquery.js.gz");
    AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/jquery.js.gz", "text/javascript");
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/CanvasJs", HTTP_GET, [](AsyncWebServerRequest* request) {
    Serial.println("Sending /CanvasJs.js.gz");
    AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/CanvasJs.js.gz", "text/javascript");
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });
  
  server.on("/Stop", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.println("stopping stream");
    request->send(204);
    streaming = false;
    sampling = false;
  });
  
  server.on("/Start", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.println("starting stream");
    request->send(204);
    sampling = true;
    streaming = true;
  });
  
}


//////////////////////////   SETUP   ////////////////////////////////////////
void setup() {
  Serial.begin(500000);


  // Setup Wifi:
  WiFi.begin(ssid, password);     //Connect to your WiFi router
  Serial.println("");
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("Wifi connected. IP: ");
  Serial.println(WiFi.localIP());
  
  configure_i2s();
  SPIFFS.begin() ? Serial.println("SPIFFS.OK") : Serial.println("SPIFFS.FAIL");


  // Setup OTA:
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      //Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
  ArduinoOTA.begin();
  delay(1000);  // allow time for OTA to break in here
  ArduinoOTA.handle();
  

  // Setup server:
  server.begin();
  serverons();
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  webSocket.begin();
  xTaskCreatePinnedToCore(getDataLoopTask, "getDataLoopTask", 80000, NULL, 0, &samplingTaskHandle, 0 );

  prev_micros = micros();
}



//////////////////////////   MAIN LOOP   /////////////////////////////////
// Handle infrastructural things in main loop.
// Sampling is handled in separate tasks outside of this loop.
void loop() {
  webSocket.loop();
  ArduinoOTA.handle();
  vTaskDelay(1);
}



/////////////////////   SAMPLING TASK AND FUNCTIONS  ////////////////////
static void getDataLoopTask(void * pvParameters){
  for( ;; ){
    //vTaskDelay(1);          // REQUIRED TO RESET THE WATCH DOG TIMER IF WORKFLOW DOES NOT CONTAIN ANY OTHER DELAY
    if(sampling){
      getSamples();
      delayMicroseconds(100); // needed less than 1-tick (1ms) delay
      esp_task_wdt_reset();  // reset watchdog timer
    } else {
      vTaskDelay(1);  // resets watchdog with 1-tick (1ms) delay
    }
  }
}

static const inline void getSamples(){
  i2s_read(i2s_port, (void*)i2s_read_buff, i2s_buffer_len*sizeof(uint16_t), &bytes_read, portMAX_DELAY);
  if(streaming && I2S_EVENT_RX_DONE && bytes_read>0){
    sendSamples();
  }
}

static const inline void sendSamples(){
  // unsigned long micros_now = micros();    // used for troubleshooting sampling rate
  //Serial.println(micros_now-prev_micros);  // used for troubleshooting sampling rate
  
  //// Per esp32.com forum topic 11023, esp32 swaps even/odd samples,
  ////   i.g. samples 0 1 2 3 4 5 are stored as 1 0 3 2 5 4 ..
  ////   Have to deinterleave manually...
  //// Also need to mask upper 4 bits which contain channel info (see gitter chat between me-no-dev and bzeeman)
  for(int i=0; i<ws_tx_buffer_len; i+=2){  // caution: this is not robust to odd buffer lens
    ws_send_buffer[i] = i2s_read_buff[i+1] & 0x0FFF;
    ws_send_buffer[i+1] = i2s_read_buff[i] & 0x0FFF;
    //Serial.printf("%04X\n",ws_send_buffer[i]);
  }

  // Send binary packet
  webSocket.sendBIN(0, (uint8_t *)&ws_send_buffer[0], ws_tx_buffer_len*sizeof(uint16_t)); 

  //prev_micros = micros_now;  // use for troubleshooting sampling rate

  // webSockets, OTA, etc infrastructural tasks are more stable with this delay here.
  vTaskDelay(1);  // non-blocking delay 1-tick (1ms);  beware, this can cause data dropouts if your buffer spans less than 1ms
}




void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  Serial.println("Async WebSocket event");
  if(type == WS_EVT_CONNECT){
    Serial.println("Async_WebSocket_Connected");
    globalClient = client;
  } else if(type == WS_EVT_DISCONNECT){
    globalClient = NULL;
    streaming = false;
    sampling = false;
    Serial.println("Async_WebSocket_Disconnected");
  } else {
    Serial.println("Unhandled Async WebSocket Event");
  }
}
