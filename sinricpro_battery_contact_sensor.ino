#include <WiFi.h>
#include "driver/adc.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_wifi.h>

//#define ENABLE_DEBUG
#define SINRICPRO_NOSSL
#define WIFI_CACHE
//#define STATIC_IP

#ifdef ENABLE_DEBUG
 #define DEBUG_ESP_PORT Serial
 #define NODEBUG_WEBSOCKETS
 #define NDEBUG
#endif

#include "SinricPro.h"
#include "SinricProContactsensor.h"

RTC_DATA_ATTR struct {
    uint8_t bssid[6];
    uint8_t channel;
} wifi_cache;

typedef enum {
    LED_ON      = HIGH ,
    LED_OFF     = LOW
} LED_State_t ;

typedef enum {
    REED_CLOSED = LOW,
    REED_OPEN   = HIGH
} Reed_State_t ;

#define BAUD_RATE 115200

#define CONTACT_ID        "6274fa311d6a67083b4ab5d1"
#define APP_KEY           "9024c456-7774-4761-b390-6e4a7c1b5ff5"   // Should look like "de0bxxxx-1x3x-4x3x-ax2x-5dabxxxxxxxx"
#define APP_SECRET        "a7a50c4a-e838-4e7a-ae73-bc9875a3c0da-6b30fe1e-d5a4-478c-9183-596604b2d5f2"   // Should look like "5f36xxxx-x3x7-4x3x-xexe-e86724a9xxxx-4c4axxxx-3x3x-x5xe-x9x3-333d65xxxxxx"
#define WIFI_SSID         "June-2G"
#define WIFI_PASS         "wifipassword"

RTC_DATA_ATTR bool volatile doorClosed  = true ;
RTC_DATA_ATTR bool volatile sendState = false ;

LED_State_t ledState ;
SinricProContactsensor &myContact = SinricPro[CONTACT_ID];

gpio_num_t const REED_PIN = GPIO_NUM_15 ;
gpio_num_t const LED_PIN  = GPIO_NUM_14 ;

static void deep_sleep_when_door_open_or_closed();
static void display_wake_up_reason( esp_sleep_wakeup_cause_t wakeup_reason ) ;
void start_wifi();
void on_wifi_connect(arduino_event_id_t event, arduino_event_info_t info);

#ifdef STATIC_IP    
  IPAddress localIP(192,168,1,124);
  IPAddress gateway(192,168,1,1);
  IPAddress subnet(255,255,255,0);
  IPAddress dns(192,168,1,1);
#endif
 
void setup() {
  //setCpuFrequencyMhz(80); //Set CPU clock to 80MHz to low the power consumption. This will take longer to establish wifi connection and send events!
  
  Serial.begin(BAUD_RATE) ;
  while(!Serial);
    
  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  // LED on
  ledState = LED_ON;
  digitalWrite(LED_PIN, ledState);
  
  Serial.printf("Send door state ? %s\n", sendState ? "Yes" : "No" ) ;

  // When the device wakes up, check if wifi is enabled, then connect and send an event to sinric pro with door state
  if (sendState) {
      start_wifi();
      SinricPro.begin(APP_KEY, APP_SECRET);  
      wait_for_sinricpro();      
      send_contact_state(); 
      stop_sinricpro(); 
      stop_wifi();
      sendState = false ;
  }

  if (doorClosed) {
      Serial.printf("setup(): door closed!.\n" ) ;
      doorClosed  = false ;
      sendState = true ;
      deep_sleep_when_door_open_or_closed() ;
  }
  else {
      Serial.printf("setup(): door opened!.\n" ) ;
      doorClosed  = true ;
      sendState = true ;
      deep_sleep_when_door_open_or_closed() ;
  }
}
    
/* Goes into deep sleep mode and wakes when the reed switch closes or open */
static void deep_sleep_when_door_open_or_closed() {
  esp_sleep_enable_ext0_wakeup(REED_PIN, doorClosed ? REED_CLOSED : REED_OPEN) ;

  //esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  //esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);

  // LED off just before
  digitalWrite(LED_PIN, LED_OFF) ;

  //adc_power_release(); // 
        
  esp_deep_sleep_start() ;

  esp_sleep_wakeup_cause_t wakeup_reason ;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  display_wake_up_reason( wakeup_reason ) ;
}
      
static void display_wake_up_reason( esp_sleep_wakeup_cause_t wakeup_reason ) {
    switch(wakeup_reason)
    {
        case ESP_SLEEP_WAKEUP_EXT0      : Serial.println("\nWakeup caused by external signal using RTC_IO") ;               break ;
        case ESP_SLEEP_WAKEUP_EXT1      : Serial.println("\nWakeup caused by external signal using RTC_CNTL") ;             break ;
        case ESP_SLEEP_WAKEUP_TIMER     : Serial.println("\nWakeup caused by timer") ;                                      break ;
        case ESP_SLEEP_WAKEUP_TOUCHPAD  : Serial.println("\nWakeup caused by touchpad") ;                                   break ;
        case ESP_SLEEP_WAKEUP_ULP       : Serial.println("\nWakeup caused by ULP program") ;                                break ;
        default                         : Serial.printf("\nWakeup was not caused by deep sleep: %d\n", wakeup_reason) ;     break ;
    }
}
 
void on_wifi_connect(arduino_event_id_t event, arduino_event_info_t info) {
   wifi_cache.channel = info.wifi_sta_connected.channel;
   memcpy(wifi_cache.bssid, info.wifi_sta_connected.bssid, sizeof(wifi_cache.bssid));
}

void start_wifi() {
    Serial.printf("start_wifi(): Setup WiFi..\n") ;    
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
#ifdef STATIC_IP    
    WiFi.config(localIP, gateway, subnet, dns);
#endif

    WiFi.onEvent(on_wifi_connect, ARDUINO_EVENT_WIFI_STA_CONNECTED);

#ifdef WIFI_CACHE
    if (wifi_cache.channel > 0) {
        Serial.printf("start_wifi(): Using cached WiFi channel ..\n") ;    
        WiFi.begin(WIFI_SSID, WIFI_PASS, wifi_cache.channel, wifi_cache.bssid);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
#elif
    WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
}

void stop_wifi(){
   // Turn on WiFi completely
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
}

void stop_sinricpro() {
  SinricPro.handle(); 
  SinricPro.stop();
  
  while (SinricPro.isConnected()) { // wait for disconnect
    SinricPro.handle();
    yield();
  }
}
 
void wait_for_sinricpro() {
  while (SinricPro.isConnected() == false) { // wait for connect
    SinricPro.handle();
    yield();
  }
  delay(100);
  Serial.printf("wait_for_sinricpro(): Connected to SinricPro ..\n"); 
}

void send_contact_state() {
  Serial.printf("send_contact_state(): Sending contact state ? %s\n", doorClosed ? "CLOSED" : "OPEN");   
  myContact.sendContactEvent(doorClosed);
  SinricPro.handle();
  Serial.printf("send_contact_state(): Sent ! \n") ;    
} 

void loop() {
  // put your main code here, to run repeatedly:
}
