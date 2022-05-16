#include <WiFi.h>
#include "driver/adc.h"
#include <esp_adc_cal.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_wifi.h>

#define ENABLE_DEBUG
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
#include "contact_sensor_battery.h"

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

#define BATTERY_VMAX 420
#define BATTERY_VMIN 330

#define CONTACT_ID        ""
#define BATTERY_ID        ""

#define APP_KEY           ""   // Should look like "de0bxxxx-1x3x-4x3x-ax2x-5dabxxxxxxxx"
#define APP_SECRET        ""   // Should look like "5f36xxxx-x3x7-4x3x-xexe-e86724a9xxxx-4c4axxxx-3x3x-x5xe-x9x3-333d65xxxxxx"
#define WIFI_SSID         ""
#define WIFI_PASS         ""

RTC_DATA_ATTR bool volatile doorClosed  = true;
RTC_DATA_ATTR bool volatile sendState = false;
RTC_DATA_ATTR bool volatile healthCheckTimerEnabled = false;

LED_State_t ledState ;
SinricProContactsensor &myContact = SinricPro[CONTACT_ID];
Battery &battery = SinricPro[BATTERY_ID];

gpio_num_t const REED_PIN = GPIO_NUM_15 ;
gpio_num_t const LED_PIN  = GPIO_NUM_14 ;

// function prototypes
static void deep_sleep_when_door_open_or_closed();
static void display_wake_up_reason( esp_sleep_wakeup_cause_t wakeup_reason ) ;
static void start_wifi();
static void on_wifi_connect(arduino_event_id_t event, arduino_event_info_t info);
static double read_battery_voltage();
static void send_battery_level();
static void report_state();
static int battery_voltage_to_percentage(double voltage);
static void led_on();
static void led_off();

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

  led_on();
  
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  display_wake_up_reason( wakeup_reason ) ; //Print the wakeup reason 

  Serial.printf("setup(): send door state to SinricPro ? %s\n", sendState ? "Yes" : "No" ) ;

  // When the device wakes up, check if sendState is enabled, then connect and send an event to sinric pro with door state
  if (sendState) {
      report_state();
      sendState = false ;
  }

  if (doorClosed) {
      Serial.printf("setup(): door closed!.\n" );
      doorClosed  = false ;
      sendState = true ;
  }
  else {
      Serial.printf("setup(): door opened!.\n" ) ;
      doorClosed  = true ;
      sendState = true ;
  } 

  led_off();
  deep_sleep_when_door_open_or_closed();  
}
 
static void report_state() {
  start_wifi();
  SinricPro.begin(APP_KEY, APP_SECRET);  
  wait_for_sinricpro();      
  send_contact_state(); 
  send_battery_level();
  stop_sinricpro(); 
  stop_wifi();
}

/* Send battery volate and precent to SinricPro */
static void send_battery_level() {
  double voltage = read_battery_voltage();
  int percent = battery_voltage_to_percentage(voltage);  
  Serial.printf("report_battery_level(): voltage: %1.3lf V (%d%%)\n", voltage, percent);
  battery.sendRangeValueEvent("voltageInstance", (float)voltage);
  battery.sendRangeValueEvent("percentageInstance", percent);
  SinricPro.handle();
}
    
/* Goes into deep sleep mode and wakes when the reed switch closes or open */
static void deep_sleep_when_door_open_or_closed() {  
  esp_sleep_enable_ext0_wakeup(REED_PIN, doorClosed ? REED_CLOSED : REED_OPEN); // When the door is open, sleep until the door is closed.
    
  //adc_power_release(); // save power?

  Serial.printf("deep_sleep_when_door_open_or_closed(): going to sleep!.\n" ) ;
  esp_deep_sleep_start(); 
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
 
static void on_wifi_connect(arduino_event_id_t event, arduino_event_info_t info) {
   wifi_cache.channel = info.wifi_sta_connected.channel;
   memcpy(wifi_cache.bssid, info.wifi_sta_connected.bssid, sizeof(wifi_cache.bssid));
}

static void start_wifi() {
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

static void stop_wifi(){
  // Turn on WiFi completely
  // https://savjee.be/2019/12/esp32-tips-to-increase-battery-life/
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
}

static void stop_sinricpro() {
  SinricPro.handle(); 
  SinricPro.stop();
  
  while (SinricPro.isConnected()) { // wait for disconnect
    SinricPro.handle();
    yield();
  }
}
 
static void wait_for_sinricpro() {
  while (SinricPro.isConnected() == false) { // wait for connect
    SinricPro.handle();
    yield();
  }
  delay(100);
  Serial.printf("wait_for_sinricpro(): Connected to SinricPro ..\n"); 
}

static void send_contact_state() {
  Serial.printf("send_contact_state(): Sending contact state ? %s\n", doorClosed ? "CLOSED" : "OPEN");   
  myContact.sendContactEvent(doorClosed);
  SinricPro.handle();
  Serial.printf("send_contact_state(): Sent ! \n") ;    
} 
 
static double read_battery_voltage() {
  uint32_t value = 0;
  int rounds = 11;
  esp_adc_cal_characteristics_t adc_chars;

  //battery voltage divided by 2 can be measured at GPIO34, which equals ADC1_CHANNEL6
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  switch(esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars)) {
    case ESP_ADC_CAL_VAL_EFUSE_TP:
      Serial.println("read_battery_voltage(): Characterized using Two Point Value");
      break;
    case ESP_ADC_CAL_VAL_EFUSE_VREF:
      Serial.printf("read_battery_voltage(): Characterized using eFuse Vref (%d mV)\r\n", adc_chars.vref);
      break;
    default:
      Serial.printf("read_battery_voltage(): Characterized using Default Vref (%d mV)\r\n", 1100);
  }

  //to avoid noise, sample the pin several times and average the result
  for(int i=1; i<=rounds; i++) {
    value += adc1_get_raw(ADC1_CHANNEL_6);
  }
  value /= (uint32_t)rounds;

  //due to the voltage divider (1M+1M) values must be multiplied by 2
  //and convert mV to V
  return (double)esp_adc_cal_raw_to_voltage(value, &adc_chars)*2.0/1000.0;
}

static int battery_voltage_to_percentage(double voltage) {
  int res = 101 - (101 / pow(1 + pow(1.33 * ((int)(voltage * 100) - BATTERY_VMIN) / (BATTERY_VMAX - BATTERY_VMIN), 4.5), 3));

  if (res >= 100)
    res = 100;

  return res;
}

static void led_on() {
  // LED on
  ledState = LED_ON;
  digitalWrite(LED_PIN, ledState); 
}

static void led_off() {
 ledState = LED_OFF;
 digitalWrite(LED_PIN, ledState) ; 
}


void loop() {
  // put your main code here, to run repeatedly:
}
