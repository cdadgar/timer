/*
 * module is a esp-1  (Generic ESP8266 Module)
 * flash size set to 1MB (FS:128KB OTA:~438KB)
 */

/*
 * todo:
 */

/*
 * library sources:
 * ESP8266WiFi, ESP8266WebServer, FS, DNSServer, Hash, EEPROM, ArduinoOTA - https://github.com/esp8266/Arduino
 * WebSocketsServer - https://github.com/Links2004/arduinoWebSockets (git)
 * WiFiManager - https://github.com/tzapu/WiFiManager (git)
 * ESPAsyncTCP - https://github.com/me-no-dev/ESPAsyncTCP (git)
 * ESPAsyncUDP - https://github.com/me-no-dev/ESPAsyncUDP (git)
 * PubSub - https://github.com/knolleary/pubsubclient (git)
 * TimeLib - https://github.com/PaulStoffregen/Time (git)
 * Timezone - https://github.com/JChristensen/Timezone (git)
 * ArduinoJson - https://github.com/bblanchon/ArduinoJson  (git)
 * FastLED - https://github.com/FastLED/FastLED (git)
 */

#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>
#include <Hash.h>
#include <TimeLib.h> 
#include <Timezone.h>

//US Eastern Time Zone (New York, Detroit)
TimeChangeRule myDST = {"EDT", Second, Sun, Mar, 2, -240};    //Daylight time = UTC - 4 hours
TimeChangeRule mySTD = {"EST", First, Sun, Nov, 2, -300};     //Standard time = UTC - 5 hours
Timezone myTZ(myDST, mySTD);

// --------------------------------------------

// web server library includes
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// --------------------------------------------

// file system (spiffs) library includes
#include <FS.h>

// --------------------------------------------

// wifi manager library includes
#include <DNSServer.h>
#include <WiFiManager.h>

WiFiManager wifiManager;
String ssid;

// --------------------------------------------

// aync library includes
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>

// --------------------------------------------

// mqtt library includes
#include <PubSubClient.h>

// --------------------------------------------

// arduino ota library includes
#include <ArduinoOTA.h>

#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
ESP8266HTTPUpdateServer httpUpdater;

// --------------------------------------------

// fastled library includes
#include <FastLED.h>

//#define DATA_PIN      0   // GPIO0
#define DATA_PIN      D1   // wemos D1
#define LED_TYPE      WS2812
#define COLOR_ORDER   GRB
#define NUM_LEDS      20

#define MILLI_AMPS         2000 // IMPORTANT: set the max milli-Amps of your power supply (4A = 4000mA)
#define FRAMES_PER_SECOND  120  // here you can control the speed. With the Access Point / Web Server the animations run a bit slower.

// Define the array of leds
CRGB leds[NUM_LEDS];

uint8_t brightness = 84;

// --------------------------------------------

const char *weekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// --------------------------------------------

bool isSetup = false;
unsigned long lastMinutes;
unsigned long lastSeconds;

#define OFF    0
#define ON     1

#define STOP   0
#define RUN    1


#define NONE         -1
#define TIME_OFF     255
#define NUM_PROGRAMS 4
#define NUM_TIMES    3


typedef struct {
  byte isEnabled;
  byte dayMask;
  byte startTime[NUM_TIMES];
  byte stopTime[NUM_TIMES];
} programType;

programType program[NUM_PROGRAMS];

int runningProgram;
int remainingMinutes;
int remainingSeconds;

ESP8266WebServer server(80);
File fsUploadFile;

WebSocketsServer webSocket = WebSocketsServer(81);
int webClient = -1;
int programClient = -1;
int setupClient = -1;

bool isTimeSet = false;

WiFiClient espClient;
PubSubClient client(espClient);

#define HOST_NAME "TIMER"
#define MQTT_IP_ADDR "192.168.1.210"
#define MQTT_IP_PORT 1883

bool isPromModified;
bool isMemoryReset = false;
//bool isMemoryReset = true;

typedef struct {
  char host_name[17];
  char mqtt_ip_addr[17];
  int mqtt_ip_port;
  byte use_mqtt;
  byte mode;
} configType;

configType config;

int state = OFF;

void loadConfig(void);
void loadProgramConfig(void);
bool setupWifi(void);
void setupTime(void);
void setupWebServer(void);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght);
void set(char *name, const char *value);
void saveProgramConfig(void);
unsigned long sendNTPpacket(IPAddress& address);
void printTime(bool isCheckProgram, bool isTest);
void printState(void);
void sendWeb(const char *command, const char *value);
void checkTimeMinutes(void);
void checkTimeSeconds(void);
void saveConfig(void);
void stopProgram(void);
void startProgram(int index, int startIndex);
void checkProgram(int day, int h, int m);
void update(int addr, byte data);
void checkRemainingRunning(void);
void printRunning(void);
void setupRelay(void);
void setupLed(void);
void relay(void);


void setup(void) {
  // start serial port
  Serial.begin(115200);
  Serial.print(F("\n\n"));

  Serial.println(F("esp8266 timer"));
  Serial.println(F("compiled:"));
  Serial.print( __DATE__);
  Serial.print(F(","));
  Serial.println( __TIME__);

  setupRelay();
  setupLed();
  
  if (!setupWifi())
    return;
    
  // must specify amount of eeprom to use (max is 4k?)
  EEPROM.begin(512);
  
  loadConfig();
  loadProgramConfig();
  isMemoryReset = false;

  setupTime();
  setupWebServer();  
  setupMqtt();
  setupOta();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
    
  lastMinutes = 0;
  lastSeconds = 0;

  runningProgram = -1;
  remainingMinutes = 0;
  remainingSeconds = 0;

  isSetup = true;
}


void setupRelay(void) {
//  pinMode(0, OUTPUT);
  pinMode(2, OUTPUT);

  state = OFF;
  relay();
}


void setupLed(void) {
   FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);         // for WS2812 (Neopixel)
  //FastLED.addLeds<LED_TYPE,DATA_PIN,CLK_PIN,COLOR_ORDER>(leds, NUM_LEDS); // for APA102 (Dotstar)
  FastLED.setDither(false);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(brightness);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MILLI_AMPS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}


void fadeall() {
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i].nscale8(250);
  }
}


void initProgram(void) {
/*
3:30am, tue and sat

each day is a bit: sun is lsb, sat is msb
mon thru fri: B00111110
sat and sun:  B01000001

time starts at 12am = 0, and goes in 15 minute increments
4:00pm: (4+12)*4
*/
  for (int i=0; i < NUM_PROGRAMS; ++i) {
    program[i].isEnabled = false;
    program[i].dayMask = B00000000;
    for (int j=0; j < NUM_TIMES; ++j) {
      program[i].startTime[j] = TIME_OFF;
      program[i].stopTime[j]  = TIME_OFF;
    }
  }
  
  program[0].isEnabled = true;
  program[0].dayMask = B00111110;      // mon thru fri
  program[0].startTime[0] = (4+12)*4;  // 4pm
  program[0].stopTime[0]  = (8+12)*4;  // 8pm

  program[1].isEnabled = true;
  program[1].dayMask = B01000001;      // sat and sun
  program[1].startTime[0] = (12)*4;    // noon
  program[1].stopTime[0]  = (8+12)*4;  // 8pm

  saveProgramConfig(); 
}


void configModeCallback(WiFiManager *myWiFiManager) {
  // this callback gets called when the enter AP mode, and the users
  // need to connect to us in order to configure the wifi
  Serial.print(F("\n\nJoin: "));
  Serial.println(config.host_name);
  Serial.print(F("Goto: "));
  Serial.println(WiFi.softAPIP());
  Serial.println();
}


bool setupWifi(void) {
  WiFi.hostname(config.host_name);
  
//  wifiManager.setDebugOutput(false);
  
  //reset settings - for testing
  //wifiManager.resetSettings();

  ssid = WiFi.SSID();
  if (ssid.length() > 0) {
    Serial.print(F("Connecting to "));
    Serial.println(ssid);
  }
  
  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  if(!wifiManager.autoConnect(config.host_name)) {
    Serial.println(F("failed to connect and hit timeout"));
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  } 

  return true;
}


void setupTime(void) {
  Serial.println(F("Getting time"));

  AsyncUDP* udp = new AsyncUDP();

  // time.nist.gov NTP server
  // NTP requests are to port 123
  if (udp->connect(IPAddress(129,6,15,28), 123)) {
//    Serial.println("UDP connected");
    
    udp->onPacket([](void *arg, AsyncUDPPacket packet) {
//      Serial.println(F("received NTP packet"));
      byte *buf = packet.data();
      
      //the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, esxtract the two words:
    
      // convert four bytes starting at location 40 to a long integer
      unsigned long secsSince1900 =  (unsigned long)buf[40] << 24;
      secsSince1900 |= (unsigned long)buf[41] << 16;
      secsSince1900 |= (unsigned long)buf[42] << 8;
      secsSince1900 |= (unsigned long)buf[43];
      time_t utc = secsSince1900 - 2208988800UL;
    
      TimeChangeRule *tcr;
      time_t local = myTZ.toLocal(utc, &tcr);
      Serial.printf("\ntime zone %s\n", tcr->abbrev);
    
      setTime(local);
    
      // just print out the time
      printTime(false, true);
    
      isTimeSet = true;

      free(arg);
    }, udp);
    
//    Serial.println(F("sending NTP packet"));

    const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
    byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold outgoing packet

    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49;
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;
    
    // all NTP fields have been given values, now
    // send a packet requesting a timestamp:
    udp->write(packetBuffer, NTP_PACKET_SIZE);
  }
  else {
    free(udp);
    Serial.println(F("\nWiFi:time failed...will retry in a minute"));
  }
}


void printState(void) {
  if (webClient != -1) {
    char buf[3];
    sprintf(buf, "%d", state);    
    sendWeb("state", buf);
  }
}


void printMode(void) {
  if (webClient != -1) {
    char buf[3];
    sprintf(buf, "%d", config.mode);    
    sendWeb("mode", buf);
  }
}


int ledIndex = 0;
int ledColorIndex = 0;
CRGB ledColor = CRGB::Red;


void loop(void)
{
  if (!isSetup)
    return;
   
  unsigned long time = millis();

  checkTimeMinutes();

  if (runningProgram != -1)
    checkTimeSeconds();

  webSocket.loop();
  server.handleClient();
  MDNS.update();
  
  // mqtt
  if (config.use_mqtt) {
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
  }

  ArduinoOTA.handle();

  EVERY_N_MILLISECONDS(75) {
    leds[ledIndex] = ledColor;
    FastLED.show();
    if (++ledIndex == NUM_LEDS) {
      ledIndex = 0;
      switch(++ledColorIndex) {
        case 1:
        case 3:
        case 5:
          ledColor = CRGB::Black;
          Serial.println("black");
          break;
        case 2:
          ledColor = CRGB::Green;
          Serial.println("green");
          break;
        case 4:
          ledColor = CRGB::Blue;
          Serial.println("blue");
          break;
        case 6:
          ledColor = CRGB::Red;
          ledColorIndex = 0;
          Serial.println("red");
          break;
      }
    }
  }
  
  // insert a delay to keep the framerate modest
//  FastLED.delay(1000 / FRAMES_PER_SECOND);
}

void checkTimeSeconds(void) {
  int seconds = second();
  if (seconds == lastSeconds)
    return;

  lastSeconds = seconds;
  checkRemainingRunning();
  printRunning();
}


void checkRemainingRunning(void) {
  // called every second
  if (runningProgram == -1)
    return;

  if (--remainingSeconds < 0) {
    remainingSeconds = 59;
    --remainingMinutes;
    if (remainingMinutes < 0)
      stopProgram();
  }
}


void printRunning(void) {
  if (runningProgram == -1)
    return;
    
  if (webClient != -1) {
    char buf[20];
    int hrs = remainingMinutes / 60;
    int mins = remainingMinutes % 60;
    sprintf(buf, "Prog %d,%2d:%02d:%02d", runningProgram+1, hrs, mins, remainingSeconds);
    sendWeb("status", buf);
  }
}


void checkTimeMinutes() {
  int minutes = minute();
  if (minutes == lastMinutes)
    return;

  // resync time at 3am every morning
  // this also catches daylight savings time changes which happen at 2am
  if (minutes == 0 && hour() == 3)
    isTimeSet = false;

  if (!isTimeSet)
    setupTime();
  
  lastMinutes = minutes;
  printTime(true, false);
}


void relay() {
  int value = (state == ON) ? HIGH : LOW;
//  digitalWrite(0, value);
  digitalWrite(2, value);
  // internal led on the wemos d1 r1
}


void sendMqtt() {
  if (config.use_mqtt) {
    char topic[30];
    sprintf(topic, "%s/state", config.host_name);
    client.publish(topic, ((state == ON) ? "on" : "off"));
  }
}


void sendWeb(const char *command, const char *value) {
  char json[128];
  sprintf(json, "{\"command\":\"%s\",\"value\":\"%s\"}", command, value);
  webSocket.sendTXT(webClient, json, strlen(json));
}


void printTime(bool isCheckProgram, bool isTest) {
  int dayOfWeek = weekday()-1;
  int hours = hour();
  int minutes = minute();

  if (webClient != -1 || isTest) {
    const char *ampm = "a";
    int h = hours;
    if (h == 0)
      h = 12;
    else if (h == 12)
      ampm = "p";
    else if (h > 12) {
      h -= 12;
      ampm = "p";
    }
    char buf[7];
    sprintf(buf, "%2d:%02d%s", h, minutes, ampm); 

    char msg[6+1+4];
    sprintf(msg, "%s %s", buf, weekdayNames[dayOfWeek]); 
    if (webClient != -1)
      sendWeb("time", msg);
    if (isTest)
      Serial.printf("time is %s\n", msg);
  }

  if (isCheckProgram) {
    if (runningProgram == -1) {
      // see if the program needs to be started
      checkProgram(dayOfWeek, hours, minutes);
    }
  }
}


void startProgram(int index, int startIndex) {
  state = ON;
  relay();
  sendMqtt();

  printState();

  runningProgram = index;

  int start = program[runningProgram].startTime[startIndex];
  int stop  = program[runningProgram].stopTime[startIndex];

  // program into the next day
  if (stop < start)
    stop += 24 * 4;

  remainingMinutes = (stop - start) * 15;
  remainingSeconds = 0;

  Serial.printf("starting program %d\n", (runningProgram+1));
}


void stopProgram(void) {
  state = OFF;
  relay();
  sendMqtt();

  runningProgram = -1;
  printState();
  if (webClient != -1)
    sendWeb("status", "");
}


void checkProgram(int day, int h, int m) {
  if (config.mode == OFF)
    return;
    
  // check each program
  int ctime = h*60+m;
  for (int i=0; i < NUM_PROGRAMS; ++i) {
    if ( !program[i].isEnabled )
      continue;
    
    // check day
    if (((1 << day) & program[i].dayMask) == 0)
      continue;
    
    // check each start time
    for (int j=0; j < NUM_TIMES; ++j) {
      if (program[i].startTime[j] == TIME_OFF)
        continue;
        
      int ptime = program[i].startTime[j]*15;
      if (ptime == ctime) {
        startProgram(i, j); 
        return;
      }
    }
  }
  
  // no programs were matches
}


#define MAGIC_NUM   0xBD

#define MAGIC_NUM_ADDRESS      0
#define CONFIG_ADDRESS         1
#define PROGRAM_ADDRESS        CONFIG_ADDRESS + sizeof(config)


void set(char *name, const char *value) {
  for (int i=strlen(value); i >= 0; --i)
    *(name++) = *(value++);
}


void loadConfig(void) {
  int magicNum = EEPROM.read(MAGIC_NUM_ADDRESS);
  if (magicNum != MAGIC_NUM) {
    Serial.println(F("invalid eeprom data"));
    isMemoryReset = true;
  }
  
  if (isMemoryReset) {
    // nothing saved in eeprom, use defaults
    Serial.println(F("using default config"));
    set(config.host_name, HOST_NAME);
    set(config.mqtt_ip_addr, MQTT_IP_ADDR);
    config.mqtt_ip_port = MQTT_IP_PORT;
    config.use_mqtt = 0;
    config.mode = OFF;

    saveConfig();
  }
  else {
    int addr = CONFIG_ADDRESS;
    byte *ptr = (byte *)&config;
    for (int i=0; i < sizeof(config); ++i, ++ptr)
      *ptr = EEPROM.read(addr++);
  }

  Serial.printf("host_name %s\n", config.host_name);
  Serial.printf("use_mqtt %d\n", config.use_mqtt);
  Serial.printf("mqqt_ip_addr %s\n", config.mqtt_ip_addr);
  Serial.printf("mqtt_ip_port %d\n", config.mqtt_ip_port);
  Serial.printf("mode %d\n", config.mode);
}


void loadProgramConfig(void) {
  if (isMemoryReset) {
    // nothing saved in eeprom, use defaults
    Serial.printf("using default programs\n");
    initProgram();  
  }
  else {
    Serial.printf("loading programs from eeprom\n");
    int addr = PROGRAM_ADDRESS;
    byte *ptr = (byte *)&program;
    for (int i = 0; i < sizeof(program); ++i, ++ptr, ++addr)
      *ptr = EEPROM.read(addr);
  }
}


void saveConfig(void) {
  isPromModified = false;
  update(MAGIC_NUM_ADDRESS, MAGIC_NUM);

  byte *ptr = (byte *)&config;
  int addr = CONFIG_ADDRESS;
  for (int j=0; j < sizeof(config); ++j, ++ptr)
    update(addr++, *ptr);
  
  if (isPromModified)
    EEPROM.commit();
}


void update(int addr, byte data) {
  if (EEPROM.read(addr) != data) {
    EEPROM.write(addr, data);
    isPromModified = true;
  }
}


void setupOta(void) {
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(config.host_name);

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    const char *msg = "Unknown Error";
    if (error == OTA_AUTH_ERROR) {
      msg = "Auth Failed";
    } else if (error == OTA_BEGIN_ERROR) {
      msg = "Begin Failed";
    } else if (error == OTA_CONNECT_ERROR) {
      msg = "Connect Failed";
    } else if (error == OTA_RECEIVE_ERROR) {
      msg = "Receive Failed";
    } else if (error == OTA_END_ERROR) {
      msg = "End Failed";
    }
    Serial.println(msg);
  });
  
  ArduinoOTA.begin();
  Serial.println("Arduino OTA ready");

  char host[20];
  sprintf(host, "%s-webupdate", config.host_name);
  MDNS.begin(host);
  httpUpdater.setup(&server);
  MDNS.addService("http", "tcp", 80);
  Serial.println("Web OTA ready");
}


void setupMqtt() {
  client.setServer(config.mqtt_ip_addr, config.mqtt_ip_port);
  client.setCallback(callback);
}


void saveProgramConfig(void) {
  isPromModified = false;
  Serial.printf("saving programs to eeprom\n");
  int addr = PROGRAM_ADDRESS;
  byte *ptr = (byte *)&program;
  for (int i = 0; i < sizeof(program); ++i, ++ptr, ++addr)
    update(addr, *ptr);

  if (isPromModified)
    EEPROM.commit();
}


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      if (num == webClient)
        webClient = -1;
      else if (num == programClient)
        programClient = -1;
      else if (num == setupClient)
        setupClient = -1;
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      }
      
      if (strcmp((char *)payload,"/") == 0) {
        webClient = num;
      
        // send the current state
        sendWeb("name", config.host_name);
        printState();
        printMode();
        printTime(false, false);
      }
      else if (strcmp((char *)payload,"/program") == 0) {
        programClient = num;
        
        // send programs
        char json[265*2];
        strcpy(json, "{\"command\":\"program\",\"value\":[");
        for (int i=0; i < NUM_PROGRAMS; ++i) {
          sprintf(json+strlen(json), "%s[%d,%d,[", (i==0)?"":",", program[i].isEnabled, program[i].dayMask);
          for (int j=0; j < NUM_TIMES; ++j)
            sprintf(json+strlen(json), "%d%s", program[i].startTime[j], (j==NUM_TIMES-1)?"],[":",");
          for (int j=0; j < NUM_TIMES; ++j)
            sprintf(json+strlen(json), "%d%s", program[i].stopTime[j], (j==NUM_TIMES-1)?"]]":",");
        }
        strcpy(json+strlen(json), "]}");
        //Serial.printf("len %d\n", strlen(json));
        webSocket.sendTXT(programClient, json, strlen(json));
      }
      else if (strcmp((char *)payload,"/setup") == 0) {
        setupClient = num;

        char json[256];
        strcpy(json, "{");
        sprintf(json+strlen(json), "\"date\":\"%s\"", __DATE__);
        sprintf(json+strlen(json), ",\"time\":\"%s\"", __TIME__);
        sprintf(json+strlen(json), ",\"host_name\":\"%s\"", config.host_name);
        sprintf(json+strlen(json), ",\"use_mqtt\":\"%d\"", config.use_mqtt);
        sprintf(json+strlen(json), ",\"mqtt_ip_addr\":\"%s\"", config.mqtt_ip_addr);
        sprintf(json+strlen(json), ",\"mqtt_ip_port\":\"%d\"", config.mqtt_ip_port);
        sprintf(json+strlen(json), ",\"ssid\":\"%s\"", ssid.c_str());
        strcpy(json+strlen(json), "}");
//        Serial.printf("len %d\n", strlen(json));
        webSocket.sendTXT(setupClient, json, strlen(json));
      }
      else {
        Serial.printf("unknown call %s\n", payload);
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\n", num, payload);
      
      if (num == webClient) {
        const char *target = "command";
        char *ptr = strstr((char *)payload, target) + strlen(target)+3;
        if (strncmp(ptr,"mode",4) == 0) {
          target = "value";
          ptr = strstr(ptr, target) + strlen(target)+3;
          config.mode = strtol(ptr, &ptr, 10);
          saveConfig();
        }        
        else if (strncmp(ptr,"state",5) == 0) {
          target = "value";
          ptr = strstr(ptr, target) + strlen(target)+3;
          state = strtol(ptr, &ptr, 10);
          relay();
          sendMqtt();
        }        
      }
      else if (num == programClient) {
        Serial.printf("save programs\n");
        char *ptr = strchr((char *)payload, '[')+2;
        for (int i=0; i < NUM_PROGRAMS; ++i) {
          program[i].isEnabled = strtol(ptr, &ptr, 10);
          ptr += 1;

          program[i].dayMask = strtol(ptr, &ptr, 10);
          ptr += 2;

          for (int j=0; j < NUM_TIMES; ++j, ++ptr) {
            program[i].startTime[j] = strtol(ptr, &ptr, 10);
//            Serial.printf("startTime %d %d %d\n", i, j, program[i].startTime[j]);
          }
          ptr += 2;
          for (int j=0; j < NUM_TIMES; ++j, ++ptr) {
            program[i].stopTime[j] = strtol(ptr, &ptr, 10);
//            Serial.printf("stopTime %d %d %d\n", i, j, program[i].stopTime[j]);
          }
          ptr += 3;
        }      
        saveProgramConfig();
      }
      else if (num == setupClient) {
        const char *target = "command";
        char *ptr = strstr((char *)payload, target) + strlen(target)+3;
        if (strncmp(ptr,"reboot",6) == 0) {
          ESP.restart();
        }
        else if (strncmp(ptr,"wifi",4) == 0) {
          wifiManager.resetSettings();
          ESP.restart();
        }
        else if (strncmp(ptr,"save",4) == 0) {
          Serial.printf("save setup\n");
          
          const char *target = "host_name";
          char *ptr = strstr((char *)payload, target) + strlen(target)+3;
          char *end = strchr(ptr, '\"');
          memcpy(config.host_name, ptr, (end-ptr));
          config.host_name[end-ptr] = '\0';

          target = "use_mqtt";
          ptr = strstr((char *)payload, target) + strlen(target)+3;
          config.use_mqtt = strtol(ptr, &ptr, 10);

          target = "mqtt_ip_addr";
          ptr = strstr((char *)payload, target) + strlen(target)+3;
          end = strchr(ptr, '\"');
          memcpy(config.mqtt_ip_addr, ptr, (end-ptr));
          config.mqtt_ip_addr[end-ptr] = '\0';

          target = "mqtt_ip_port";
          ptr = strstr((char *)payload, target) + strlen(target)+3;
          config.mqtt_ip_port = strtol(ptr, &ptr, 10);

          Serial.printf("host_name %s\n", config.host_name);
          Serial.printf("use_mqtt %d\n", config.use_mqtt);
          Serial.printf("mqtt_ip_addr %s\n", config.mqtt_ip_addr);
          Serial.printf("mqtt_ip_port %d\n", config.mqtt_ip_port);
          saveConfig();
        }
      }
      break;
  }
}


//format bytes
String formatBytes(size_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1024 * 1024)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1024 * 1024 * 1024)){
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}


String getContentType(String filename){
  if(server.hasArg("download")) return "application/octet-stream";
  else if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}


bool handleFileRead(String path){
  Serial.println("handleFileRead: " + path);
  if(path.endsWith("/")) path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}


void handleFileUpload_edit(){
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    Serial.print("handleFileUpload Name: "); Serial.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    //Serial.print("handleFileUpload Data: "); Serial.println(upload.currentSize);
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
    Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
  }
}


void handleFileDelete(){
  if(server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileDelete: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}


void handleFileCreate(){
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileCreate: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}


void handleFileList() {
  if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  
  String path = server.arg("dir");
  Serial.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  
  output += "]";
  server.send(200, "text/json", output);
}


void countRootFiles(void) {
  int num = 0;
  size_t totalSize = 0;
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    ++num;
    String fileName = dir.fileName();
    size_t fileSize = dir.fileSize();
    totalSize += fileSize;
    Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
  }
  Serial.printf("FS File: serving %d files, size: %s from /\n", num, formatBytes(totalSize).c_str());
}


void setupWebServer(void) {
  SPIFFS.begin();

  countRootFiles();

  //list directory
  server.on("/list", HTTP_GET, handleFileList);
  
  //load editor
  server.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.htm")) server.send(404, "text/plain", "FileNotFound");
  });
  
  //create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  
  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload_edit);

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if(!handleFileRead(server.uri())) {
      server.send(404, "text/plain", "FileNotFound");
    }
  });

  server.begin();

  Serial.println("HTTP server started");
}


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
//    // Create a random client ID
//    String clientId = "ESP8266Client-";
//    clientId += String(random(0xffff), HEX);
//    // Attempt to connect
//    if (client.connect(clientId.c_str())) {
    if (client.connect(config.host_name)) {
      Serial.println("connected");
      // ... and resubscribe
      char topic[30];
      sprintf(topic, "%s/state", config.host_name);
      client.subscribe(topic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


void callback(char* topic, byte* payload, unsigned int length) {
  // only topic we get is <host_name>/state

  // strip off the hostname from the topic
  topic += strlen(config.host_name) + 1;

  char value[12];
  memcpy(value, payload, length);
  value[length] = '\0';
  Serial.printf("Message arrived [%s] %s\n", topic, value);

  if (strcmp(topic, "state") == 0) {
    if (strcmp(value, "on") == 0) {
      state = ON;
    }
    else if (strcmp(value, "off") == 0) {
      state = OFF;
    }
    else {
      Serial.printf("Unknown command\n");
      return;        
    }   

    relay();
    printState();
  }
  else {
    Serial.printf("Unknown topic\n");
  }
}


/*
programs will only start when start time is hit...(if mode==run), need to be able to start
in the middle of a program, and calculate correct remaining time.

not getting tx output when connected to module...light is flashing...what gives?

crashed when starting program
add capacitors?
 */
