/*
 * module is a esp-1
 * flash size set to 1M (128K SPIFFS)
 * 
 * during upload, I sometimes get a few
 * LmacRxBlk:1
 * messages....they don't seem to cause a problem
 * 
 * after upload, you should see: (for a good reboot)
Update Success: 296592
Rebooting...

 ets Jan  8 2013,rst cause:2, boot mode:(3,6)

load 0x4010f000, len 1264, room 16 
tail 0
chksum 0x42
csum 0x42
@cp:0
ld


this also happens:
Update Success: 296592
Rebooting...

 ets Jan  8 2013,rst cause:2, boot mode:(1,7)


 ets Jan  8 2013,rst cause:4, boot mode:(1,7)

wdt reset

and the device hangs until power cycled

what does the boot mode refer to?  are the pins being pulled up/down correctly? (with resistors)
 */


/*
 * Espalexa - https://github.com/Aircoookie/Espalexa (git)
 */

#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>
#include <Hash.h>
#include <TimeLib.h> 
//#include <Timezone.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// --------------------------------------------

#include <ESP8266WebServer.h>
#include <FS.h>

// --------------------------------------------

// wifi manager includes
#include <DNSServer.h>
#include <WiFiManager.h>

// --------------------------------------------

// aync library includes
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>

// --------------------------------------------

// amazon alexa support
#include <Espalexa.h>

// --------------------------------------------

//US Eastern Time Zone (New York, Detroit)
//TimeChangeRule myDST = {"EDT", Second, Sun, Mar, 2, -240};    //Daylight time = UTC - 4 hours
//TimeChangeRule mySTD = {"EST", First, Sun, Nov, 2, -300};     //Standard time = UTC - 5 hours
//Timezone myTZ(myDST, mySTD);

const char *weekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// --------------------------------------------

bool isSetup = false;
unsigned long lastMinutes;
unsigned long lastSeconds;

#define OFF    0
#define ON     1
#define RUN    2
const char *modeNames[] = { "Off", "On", "Running" };


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

Espalexa espalexa;
ESP8266WebServer server(80);
File fsUploadFile;
bool isUploading;

WebSocketsServer webSocket = WebSocketsServer(81);
int webClient = -1;
int programClient = -1;
int setupClient = -1;

bool isTimeSet = false;

bool isPromModified;
bool isMemoryReset = false;
//bool isMemoryReset = true;

typedef struct {
  byte mode;
} configType;

configType config;

void loadConfig(void);
void loadProgramConfig(void);
bool setupWifi(void);
void setupTime(void);
void setupWebServer(void);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght);
void drawMainScreen(void);
void set(char *name, const char *value);
void saveProgramConfig(void);
unsigned long sendNTPpacket(IPAddress& address);
void printTime(bool isCheckProgram, bool isTest);
void printModeState(void);
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
void relay(bool);
void modeChange(void);


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
  
  if (!setupWifi())
    return;
    
  // must specify amount of eeprom to use (max is 4k?)
  EEPROM.begin(512);
  
  loadConfig();
  loadProgramConfig();
  isMemoryReset = false;

  setupTime();
  setupWebServer();  

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
    
  lastMinutes = 0;
  lastSeconds = 0;

  runningProgram = -1;
  remainingMinutes = 0;
  remainingSeconds = 0;
  
  isUploading = false;

  isSetup = true;
  modeChange();
}

void setupRelay(void) {
  pinMode(0, OUTPUT);
  pinMode(2, OUTPUT);
  relay(false);
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

#define AP_NAME "Timer"

void configModeCallback(WiFiManager *myWiFiManager) {
  // this callback gets called when the enter AP mode, and the users
  // need to connect to us in order to configure the wifi
  Serial.print(F("\n\nJoin: "));
  Serial.println(AP_NAME);
  Serial.print(F("Goto: "));
  Serial.println(WiFi.softAPIP());
  Serial.println();
}

bool setupWifi(void) {
  WiFi.hostname("timer");
  
  WiFiManager wifiManager;
//  wifiManager.setDebugOutput(false);
  
  //reset settings - for testing
  //wifiManager.resetSettings();

  String ssid = WiFi.SSID();
  if (ssid.length() > 0) {
    Serial.print(F("Connecting to "));
    Serial.println(ssid);
  }
  
  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  if(!wifiManager.autoConnect(AP_NAME)) {
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
    
    // cpd..hack until timezone is fixed
//      utc -= 60 * 60 * 4;  // spring foward
      utc -= 60 * 60 * 5;  // fall back
      setTime(utc);
    
//      TimeChangeRule *tcr;
//      time_t local = myTZ.toLocal(utc, &tcr);
//      Serial.printf("\ntime zone %s\n", tcr->abbrev);
//    
//      setTime(local);
    
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


void drawMainScreen(void) {
  printTime(true, false);
  printModeState();
}


void printModeState(void) {
  if (webClient != -1) {
    const char *msg;
    if (config.mode == OFF)
      msg = "Off";
    else if (config.mode == ON)
      msg = "On";
    else if (runningProgram == -1)
      msg = "Running";
    else
      msg = "On";

    char buf[3];
    sprintf(buf, "%d", config.mode);    
    sendWeb("mode", buf);
    sendWeb("modeState", msg);
  }
}

void loop(void)
{
  if (isUploading) {
    // you can omit this line from your code since it will be called in espalexa.loop()
    // server.handleClient();
    espalexa.loop();
    return;
  }
  
  if (!isSetup)
    return;
   
  unsigned long time = millis();

  checkTimeMinutes();

  if (runningProgram != -1)
    checkTimeSeconds();

  webSocket.loop();
  
  // you can omit this line from your code since it will be called in espalexa.loop()
  // server.handleClient();
  espalexa.loop();
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

void relay(bool isOn) {
  int value = isOn ? HIGH : LOW;
  digitalWrite(0, value);
  digitalWrite(2, value);
}

void modeChange(void) {
  saveConfig();
  if (config.mode == OFF) {
    stopProgram();
  }
  else if (config.mode == ON) {
    relay(true);
  }

  drawMainScreen();
  printModeState();
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
  relay(true);

  printModeState();

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
  relay(false);

  runningProgram = -1;
  printModeState();
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

#define MAGIC_NUM   0xAD

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
    config.mode = OFF;

    saveConfig();
  }
  else {
    int addr = CONFIG_ADDRESS;
    byte *ptr = (byte *)&config;
    for (int i=0; i < sizeof(config); ++i, ++ptr)
      *ptr = EEPROM.read(addr++);
  }

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
        printModeState();
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
          modeChange();
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
        drawMainScreen();
      }
      else if (num == setupClient) {
        const char *target = "command";
        char *ptr = strstr((char *)payload, target) + strlen(target)+3;
        if (strncmp(ptr,"reboot",6) == 0) {
          ESP.restart();
        }
        else if (strncmp(ptr,"save",4) == 0) {
          Serial.printf("save setup\n");
          
          saveConfig();
        }
      }
      break;
  }
}

void setUpOta(void) {
  server.on("/update", HTTP_POST, [](){
    server.sendHeader("Connection", "close");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
    ESP.restart();
  },[](){
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_START){
      Serial.setDebugOutput(true);
      Serial.printf("Update filename: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if(!Update.begin(maxSketchSpace)){//start with max available size
        Update.printError(Serial);
      }
    } else if(upload.status == UPLOAD_FILE_WRITE){
      Serial.printf("uploaded: %d\n", upload.totalSize);
      if(Update.write(upload.buf, upload.currentSize) == upload.currentSize){
        // update the total percent complete on the web page
        // cpd
      }
      else {
        Update.printError(Serial);
      }
    } else if(upload.status == UPLOAD_FILE_END){
      if(Update.end(true)){ //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      Serial.setDebugOutput(false);
    }
    yield();
  });
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

void timerChanged(uint8_t brightness) {
  Serial.print("Alexa changed to ");
  Serial.println(brightness);
    
  if (brightness == 255) {
    config.mode = ON;
  }
  else if (brightness == 0) {
    config.mode = OFF;
  }
  else {
    // dim...not supported
    return;
  }
  modeChange();
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

  setUpOta();

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if(!handleFileRead(server.uri())) {
      // if you don't know the URI, ask espalexa whether it is an Alexa control request
      if (!espalexa.handleAlexaApiCall(server.uri(),server.arg(0))) {
        server.send(404, "text/plain", "FileNotFound");
      }
    }
  });

  // alexa setup
  // simplest definition, default state off
  espalexa.addDevice("My Timer", timerChanged);

  // give espalexa a pointer to your server object so it can use your server
  // instead of creating its own
  espalexa.begin(&server);
  //server.begin(); //omit this since it will be done by espalexa.begin(&server)

  Serial.println("HTTP and alexa server started");
}


/*
programs will only stat when start time is hit...need to be able to start
in the middle of a prgram, and calculate correct remaining time.

not getting tx output when connected to module...light is flashing...what gives?

crashed when starting program
add capacitors?
 */
