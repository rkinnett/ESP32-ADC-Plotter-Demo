#include <Arduino.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebAuthentication.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#define FS_NO_GLOBALS
#include <FS.h>
#include "SPIFFS.h"
#include <driver/i2s.h>
#include <driver/adc.h>

// Need this to fix I2S inversion thing..
extern "C" {
    #include "soc/syscon_reg.h"
    #include "soc/syscon_struct.h"
}

//SSID and Password of your WiFi router
#include "my_wifi.h"
//const char* ssid = "YOUR_WIFI_SSID";
//const char* password = "YOUR_WIFI_PW";

AsyncWebServer    server(80);
WebSocketsServer  webSocket = WebSocketsServer(81);
AsyncEventSource  events("/events");
AsyncWebServerRequest *request;
AsyncWebSocket    ws("/test");


/** CONFIG **/
i2s_port_t I2S_NUM = I2S_NUM_0;   // I2S Port 
adc1_channel_t ADC_CHANNEL = ADC1_CHANNEL_5; //GPIO33  (SET THIS FOR YOUR HARDWARE)
const int DUMP_INTERVAL = 1;           // dump samples at this interval, msec
const int NUM_SAMPLES = 1024;
const int samplingFrequency = 44100;


/** GLOBALS **/
uint32_t collected_samples = 0;
uint32_t last_sample_count = 0;
uint16_t adc_reading;
TaskHandle_t TaskHandle_2;
int32_t  last_millis = DUMP_INTERVAL;
long Start_Sending_Millis = 0;
boolean streaming       = true;   // ADC enabler from web

uint16_t* i2s_read_buff = (uint16_t*)calloc(NUM_SAMPLES, sizeof(uint16_t));
uint16_t* adc_data = (uint16_t*)calloc(NUM_SAMPLES, sizeof(uint16_t));
size_t bytes_read;

AsyncWebSocketClient * globalClient = NULL;


// Prototypes:
void configure_i2s();
static const inline void SendData();
static const inline void Sampling();
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len);


void serverons(){

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    while(!SPIFFS.exists("/index.html")){
      Serial.println("Error, /index.html is not onboard");
      delay(1000);
    }
    Serial.println("trying to send index.html");
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
  });
  
  server.on("/Start", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.println("starting stream");
    request->send(204);
    streaming = true;
  });
  
}



void setup() {
  Serial.begin(500000);

  //WIFI_Setup();
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
  delay(2000);  // allow time for OTA to break in here

  
  server.begin();
  serverons();
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  webSocket.begin();
  xTaskCreatePinnedToCore ( loop0, "v_getData", 80000, NULL, 0, &TaskHandle_2, 0 );
  Start_Sending_Millis  = millis();
}


void loop() {
  webSocket.loop();       // WEBSOCKET PACKET LOOP
  ArduinoOTA.handle();
  vTaskDelay(1);
}



static void loop0(void * pvParameters){
  for( ;; ){
    vTaskDelay(1);          // REQUIRED TO RESET THE WATCH DOG TIMER IF WORKFLOW DOES NOT CONTAIN ANY OTHER DELAY
    Sampling();
  }
}


void configure_i2s(){
  i2s_config_t i2s_config = 
    {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),  // I2S receive mode with ADC
    .sample_rate = samplingFrequency,                                             // 144000
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,                                 // 16 bit I2S
    .channel_format = I2S_CHANNEL_FMT_ALL_LEFT,                                   // all the left channel
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),   // I2S format
    .intr_alloc_flags = 0,                                                        // none
    .dma_buf_count = 2,                                                           // number of DMA buffers 2 for fastness
    .dma_buf_len = 1024,                                                   // number of samples
    .use_apll = 0,                                                                // no Audio PLL
  };
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_11db);
  adc1_config_width(ADC_WIDTH_12Bit);
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
 
  i2s_set_adc_mode(ADC_UNIT_1, ADC_CHANNEL);
  SET_PERI_REG_MASK(SYSCON_SARADC_CTRL2_REG, SYSCON_SARADC_SAR1_INV);  //fixes inversion problem
  i2s_adc_enable(I2S_NUM_0);
  vTaskDelay(1000);
}



void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  Serial.println("Async WebSocket event");
  if(type == WS_EVT_CONNECT){
    Serial.println("Async_WebSocket_Connected");
    globalClient = client;
  } else if(type == WS_EVT_DISCONNECT){
    globalClient = NULL;
    Serial.println("Async_WebSocket_Disconnected");
  } else {
    Serial.println("Async WebSocket Event: " + type);
  }
}



static const inline void SendData(){
  String data;
  long Millis_Now = millis();

  //Serial.println("plotting");
  if ((Millis_Now-Start_Sending_Millis) >= DUMP_INTERVAL && streaming)
  {
    for(int i=0; i<bytes_read/2; i++){
      adc_data[i] = i2s_read_buff[i] & 0xFFF;
    }
 
    Start_Sending_Millis = Millis_Now;

    // SEND METHOD 1:  ASCII/JSON
    /*
    //Serial.println("sending " + String(bytes_read/2) + " samples now");
    for(int i=0; i<bytes_read/2; i++) {
      if(i>0) {data+=",";}
      data += String((adc_data[i] & 0xFFF));
    }
    webSocket.broadcastTXT(data.c_str(), data.length());
    Serial.println(data);
    */

    // SEND METHOD 2:  BINARY
    webSocket.sendBIN(0, (uint8_t *)&adc_data[0], bytes_read); // sends bytes_read bytes, one byte at a time
  }
}



static const inline void Sampling(){
  i2s_read(I2S_NUM_0, (void*)i2s_read_buff, NUM_SAMPLES * sizeof(uint16_t), &bytes_read, portMAX_DELAY);
   if(I2S_EVENT_RX_DONE && bytes_read>0){
     SendData();
   }
}
