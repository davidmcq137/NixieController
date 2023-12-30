/* 
 * Project: Nixie Clock for Dalibor Farny tubes
 * Date: Dec 2023
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/firmware/best-practices/firmware-template/
 */

// Include Particle Device OS APIs
#include "Particle.h"
#include "Encoder.h"

//#include "Adafruit_Si7021.h"
#include "RTClibrary.h"

// Let Device OS manage the connection to the Particle Cloud
SYSTEM_MODE(AUTOMATIC);

// Run the application and system concurrently in separate threads
SYSTEM_THREAD(ENABLED);

// Show system, cloud connectivity, and application logs over USB
// View logs with CLI using 'particle serial monitor --follow'
//SerialLogHandler logHandler(LOG_LEVEL_INFO);

unsigned counter;
char timebuf[7];
char timebuflast[] = "000000";
char datebuf[7];
char tempbuf[7];
char Htidebuf[7];
char Ltidebuf[7];
char spibuf[4];
int lastTime;
String tideDay = "0000-00-00";

#define ONE_DAY_MILLIS (24 * 60 * 60 * 1000)
#define DS3231Addr 0x68
#define PWMFREQ (200)
#define EEPROMOFFSET (10)

unsigned long lastSync = millis();
// RTC lib docs: https://adafruit.github.io/RTClib/html/index.html
RTC_DS3231 rtc;
//Adafruit_Si7021 Si7021;
Encoder Enc(A5, S4);

time32_t unixTime;
DateTime now;
bool rtcValid;
bool rtcAvailable;
int16_t temp = 0;
int16_t hum = 0;
int oldPosition  = -999;  
int dutyCycle = 255;
unsigned long lastmicro = micros();
float loopTime = 0.0;
float lastLoop = -1.0;
unsigned long spistart;
unsigned long spiend;
int spidelta = 0;
bool startup = true;
int colons = 0xFF;

#define NONVOLVERSION 1

struct EE {
  uint8_t version;
  int offsetFromGMT;
  bool fadeDigits;
  bool countback;
  bool dateDisp;
  bool fadeDim;
  bool tideDisp;
};

EE nonVol;

struct tiderec {
  int imins;
  float height;
  char type;
};

tiderec tidearr[4];

typedef enum {
  STATE_DISPLAY_TIME,
  STATE_SLOT_MACHINE,
  STATE_DISPLAY_DATE,
  STATE_DISPLAY_TEMP,
  STATE_HIGH_TIDE,
  STATE_LOW_TIDE
} my_state_t;

my_state_t the_state;

const size_t UART_TX_BUF_SIZE = 20;
void onDataReceived(const uint8_t *data, size_t len, const BlePeerDevice &peer, void *context);
void execKwd(String k, String v);
void execCmd(String k, String v);

String textacc;

// These UUIDs were defined by Nordic Semiconductor and are now the defacto standard for
// UART-like services over BLE. Many apps support the UUIDs now, like the Adafruit Bluefruit app.

const BleUuid serviceUuid("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
const BleUuid rxUuid("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
const BleUuid txUuid("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

BleCharacteristic txCharacteristic("tx", BleCharacteristicProperty::NOTIFY, txUuid, serviceUuid);
//BleCharacteristic rxCharacteristic("rx", BleCharacteristicProperty::WRITE_WO_RSP, rxUuid, serviceUuid, onDataReceived, NULL);
BleCharacteristic rxCharacteristic("rx", BleCharacteristicProperty::WRITE, rxUuid, serviceUuid, onDataReceived, NULL);
//2


void testHandler(const char *event, const char *data){
  Serial.printlnf("testHandler >>%s<< >>%s<<", event, data);
  
  for (int i=0; i < 3; i++) {
    tidearr[i].imins = 0;
    tidearr[i].height = 0.0;
    tidearr[i].type = 'X';
  }

  JSONValue outerobj = JSONValue::parseCopy(data);
  JSONObjectIterator iter(outerobj);
  while (iter.next()) {
    //Serial.printlnf("key=%s value=%s", (const char *)iter.name(), (const char *)iter.value().toString());
    if (iter.value().isArray()) {
      //Serial.println("Value is array");
      JSONArrayIterator ater(iter.value());
      for (size_t ii = 0; ater.next(); ii++) {
        //Serial.printlnf("%u: %s", ii, ater.value().toString().data());
        JSONObjectIterator eter(ater.value());
        int imins = 0;
        double v = 0.0;
        char typc = 'x';
        while (eter.next()) {
          //Serial.printlnf("key=%s value=%s", (const char *)eter.name(), (const char *)eter.value().toString());
          if (eter.name() == "t") {
            String datetime = eter.value().toString().data();
            if (ii == 0) {
              tideDay = datetime.substring(0, 10);
              //Serial.printlnf("Handler: tideDay %s", tideDay.c_str());
            }
            String time = datetime.substring(datetime.length() - 5);
            int colpos = time.indexOf(':');
            String mins = time.substring(colpos+1);
            String hours = time.substring(0, colpos);
            imins = mins.toInt() + 60 * hours.toInt();
            //Serial.printlnf("time: %s h: %s m: %s imins: %d", time.c_str(), hours.c_str(), mins.c_str(), imins);
          } else if (eter.name() == "v") {
            v = eter.value().toDouble();
          } else if (eter.name() == "type") {
            String typ = eter.value().toString().data();
            typc = typ.charAt(0);
          }
        }
        tidearr[ii].imins = imins;
        tidearr[ii].height = v;
        tidearr[ii].type = typc;
        Serial.printlnf("%d imins %d height %.2f type %d", ii, tidearr[ii].imins, tidearr[ii].height, tidearr[ii].type);
      } 
    }  
  }
}

// setup() runs once, when the device is first turned on
void setup() {
  Serial.begin();
  //waitFor(Serial.isConnected, 10000);

  counter = 0;
  lastTime = -1;
    
  // Put initialization like pinMode and begin functions here
  SPI.begin(SPI_MODE_MASTER, A5);
  SPI.setClockDivider(SPI_CLOCK_DIV256);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);

  // this is the FFStr (strobe) signal
  pinMode(D6, OUTPUT);
  digitalWrite(D6, LOW);

  //this is the PWM signal for brightness control
  pinMode(A2, OUTPUT);
  //analogWrite(A2, dutyCycle, PWMFREQ);
  analogWrite(A2, dutyCycle, 0); //begin with power off until valid data loaded  
    
  // rtc.begin() always returns true, even if no rtc device. heaven knows why.
  // so check manually by talking directly to the i2c bus if it's there  
  // at the expected address
  
  Wire.setSpeed(100000);
  Wire.begin();
  Wire.beginTransmission(DS3231_ADDRESS);
  int8_t bb = Wire.endTransmission();
  Serial.printlnf("endTransmission code %d", bb);
  if ( (bb == 0) || (bb == 5)) { // succeeded (0) or succeeded then busy (5)
    rtcAvailable = true;
  } else {
    rtcAvailable = false;
  }
  Serial.printlnf("rtcAvailable %d", rtcAvailable);
  Wire.end(); // close bus connection, rtc lib will reopen

  rtc.begin();
  
  if (rtcAvailable) {
    rtcValid = !rtc.lostPower();
  } else {
    rtcValid = false;
  }
  Serial.printlnf("rtcValid %d", rtcValid);

  if (rtcValid) {
    the_state = STATE_DISPLAY_TIME;
  } else {
    the_state = STATE_SLOT_MACHINE;
  }

  // Start the i2c temp and humidity sensor
  //Si7021.begin();  

  char devname[80];
  String devID = System.deviceID();
  String devshort = devID.substring(devID.length() - 6);
  sprintf(devname, "Nixie Clock %s", devshort.c_str());
  Serial.println(devname);

  // Start the Bluetooth service and begin advertising

  BLE.setDeviceName(devname);
  BLE.on();
  BLE.addCharacteristic(txCharacteristic);
  BLE.addCharacteristic(rxCharacteristic);
  BleAdvertisingData data;
  data.appendServiceUUID(serviceUuid);
  BLE.advertise(&data);

  if (EEPROM.hasPendingErase()) {
    Serial.println("has pending erase");
  }

  EEPROM.get(EEPROMOFFSET, nonVol);

  Serial.printlnf("EEPROM Version %d GMT Offset %d", nonVol.version, nonVol.offsetFromGMT);
  
  if (nonVol.version == 255) { //uninitialized EEPROM returns 255 for each byte
    nonVol.version = 1;
    nonVol.offsetFromGMT = -5;
    nonVol.fadeDigits = true;
    nonVol.fadeDim = false;
    nonVol.countback = false;
    nonVol.dateDisp = false;
    nonVol.tideDisp = false;
    Serial.printlnf("setting EEPROM first time, offsetFromGMT %d", nonVol.offsetFromGMT);
    EEPROM.put(EEPROMOFFSET, nonVol);    
  } 
  //Still need this even with RTC since particle OS won't give Time.isValid() until TZ set
  Time.zone(nonVol.offsetFromGMT);

  Particle.subscribe("hook-response/hilo", testHandler, MY_DEVICES);
}

void onLinefeed(String msg)
{
  int lparen;
  int rparen;
  int colon;
  String key;
  String val;
  bool isKwd = false;

  lparen = msg.indexOf("(");
  if (lparen < 0)
  {
    isKwd = true;
    rparen = msg.length() - 1;
  }
  else
  {
    rparen = msg.indexOf(")");
  }
  colon = msg.lastIndexOf(":");
  
  if (colon < 0)
  {
    key = msg.substring(lparen + 1, rparen);
    val = "";
  }
  else
  {
    key = msg.substring(lparen + 1, colon);
    val = msg.substring(colon + 1, rparen);
  }

  textacc = "";

  if (isKwd)
  {
    execKwd(key, val);
  }
  else
  {
    execCmd(key, val);
  }
}

//7
void onDataReceived(const uint8_t *data, size_t len, const BlePeerDevice &peer, void *context) {
  //Log.trace("Received data from: %02X:%02X:%02X:%02X:%02X:%02X:", peer.address()[0], peer.address()[1], peer.address()[2], peer.address()[3], peer.address()[4], peer.address()[5]);
  //Serial.print(len);
  //Serial.println("in onDataReceived");

  for (size_t ii = 0; ii < len; ii++)
  {
    textacc.concat(char(data[ii]));
    if (data[ii] == '\n')
    {
      onLinefeed(textacc.c_str());
    }
  }
}

void sendBLE(String str)
{
  uint8_t txbuf[200];
  String buf = str += "\r\n";
  size_t nch = buf.length() ;

  for (size_t ii = 0; (ii < nch) && (ii < 200); ii++)
  {
    txbuf[ii] = (int)str.charAt(ii);
  }
  txCharacteristic.setValue(txbuf, nch);
}

String wifissid = "";
String wifipwd = "";

void execKwd(String k, String v)
{
  String tmp = "";

  Serial.printlnf("execKwd: k,v = #%s#,#%s#", k.c_str(), v.c_str());
  
  if (k == "PWM") {
    int setPWM = v.toInt();
    if (setPWM > 255) {
      setPWM = 255;
    } 
    if (setPWM < 0) {
      setPWM = 0;
    }
    sendBLE("Set PWM");
    Serial.printlnf("PWM command %d", (int)setPWM);
    analogWrite(A2, (int)setPWM, PWMFREQ);
  } else if (k == "fade") {
    if (v == "digits") {
      nonVol.fadeDigits = true;
      nonVol.fadeDim = false;
      sendBLE("fade digits on");
    } else if (v == "dimming") {
      nonVol.fadeDim = true;
      nonVol.fadeDigits = false;
      sendBLE("fade dimming on");      
    } else {
      sendBLE("fade off");
      nonVol.fadeDigits = false;
      nonVol.fadeDim = false;
    }
  } else if (k == "countback") {
    if (v == "on") {
      nonVol.countback = true;
      sendBLE("countback on");
    } else {
      nonVol.countback = false;
      sendBLE("countback off");
    }
  } else if (k == "date") {
    if (v == "on") {
      nonVol.dateDisp = true;
      sendBLE("date disp on");
    } else {
      nonVol.dateDisp = false;
      sendBLE("date disp off");
    }
  } else if (k == "tide") {
    if (v == "on") {
      nonVol.tideDisp = true;
      sendBLE("tide disp on");
    } else {
      nonVol.tideDisp = false;
      sendBLE("tide disp off");
    }

  } else if (k == "gmtOffset") {
    int temp;
    temp =  v.toInt();
    if (temp > 24) {
      temp = 24;
    } 
    if (temp < -24) {
      temp = -24;
    }
    nonVol.offsetFromGMT = temp;
    Time.zone(nonVol.offsetFromGMT);    
    sendBLE("gmtOffset changed - set at top of next minute");
  } else if (k == "ssid") {
    wifissid = v;
    sendBLE(wifissid);
    //wifissid.concat(v);
  }
  else if (k == "pwd")
  {
    wifipwd = v;
    sendBLE(wifipwd);
    //wifipwd.concat(v);
  }
  else if (k == "clearEE"){
    nonVol.version = 255;
    EEPROM.put(EEPROMOFFSET, nonVol);    
    sendBLE("EEPROM cleared");
  }
  else if (k == "unixT")
  {
    //use Datetime constructor, convert v value to 32 bit int
    //don't forget to set time zone .. variable offsetFromGMT AND the os API call
  }
  else if (k == "ISO8601")
  {
    //use Datetime constructor directly on v assumed to be in ISO 8601 format
    //don't forget to set time zone .. variable offsetFromGMT AND the os API call
  }
  else if (k == "updateWiFi")
  {
    bool saveFade = nonVol.fadeDigits;
    bool saveDim = nonVol.fadeDim;
    nonVol.fadeDigits = false;
    nonVol.fadeDim = false;
    sendBLE("Setting wifi creds " + wifissid + " " + wifipwd);
    bool wifiClear = WiFi.clearCredentials();
    Serial.printlnf("wificlear %d", wifiClear);

    Particle.disconnect();
    WiFi.setCredentials(wifissid, wifipwd, WPA);
    WiFi.on();      //prob not required
    WiFi.connect(); //prob not required since clock runs SYSTEM AUTOMATIC

    int wLoops = 0;
    while (!WiFi.ready() && wLoops < 300){ //timeout 30 seconds if no wifi or bad creds
      delay(100);
      wLoops = wLoops + 1;
    }
    if (wLoops >= 300) {
      sendBLE("No WiFi Connection"); //No WiFi connection
      nonVol.fadeDigits = saveFade;
      return;
    }
    sendBLE("WiFi connected"); // WiFi connected
    Particle.connect();
    wLoops = 0;
    while (!Particle.connected() && wLoops < 300)
    {
      delay(100);
      wLoops = wLoops + 1;
    }
    if (wLoops >= 300) {
      sendBLE("No Cloud Connection"); // No Particle Cloud Connection 
      nonVol.fadeDigits = saveFade;
      nonVol.fadeDim = saveDim;
      return;
    }
    sendBLE("Cloud Connected"); // particle cloud connected
    nonVol.fadeDigits = saveFade;
    nonVol.fadeDim = saveDim;
  } else {
    sendBLE("Commands are: fade:digits|dimming|off, countback:on|off, date:on|off, tide:on|off, gmtOffset:hh, clearEE, ssid:ss, pwd:pp, updateWiFi");
  }
  //save into EEPROM
  EEPROM.put(EEPROMOFFSET, nonVol);

}

void execCmd(String k, String v)
{
  Serial.printlnf("execCmd: k,v = %s,%s", k.c_str(), v.c_str());
}

int writetime() {
  if (rtcValid) {
    now = rtc.now();
    return snprintf(timebuf, 7, "%02d%02d%02d", now.second(), now.minute(), now.hour());
  } else {
    return snprintf(timebuf, 7, "%02d%02d%02d", Time.second(), Time.minute(), Time.hour());
  }
}

int writedate() {
  if (rtcValid) {
    now = rtc.now();
    return snprintf(datebuf, 7, "%02d%02d%02d", now.year() % 100, now.day(), now.month());
  } else {
    return snprintf(datebuf, 7, "%02d%02d%02d", Time.year() % 100, Time.day(), Time.month());
  }
}

int writeHtide() {
  if (Particle.connected()) {
    int nmins = Time.hour() * 60 + Time.minute();
    int isave = -1;
    for(int ii = 0; ii < 4; ii++) {
      if ((tidearr[ii].imins > nmins) && tidearr[ii].type == 'H') {
        isave = ii;
        break;
      }
    }
    if (isave >= 0) {
      int hours = tidearr[isave].imins / 60;
      int mins = tidearr[isave].imins - 60 * hours;
      return snprintf(Htidebuf, 7, "%02d%02d%02d", 0, mins, hours);  
    } else {
      return snprintf(Htidebuf, 7, "::::::");
    }
  } else {
    return snprintf(Htidebuf, 7, "::::::");
  }
}

int writeLtide() {
  if (Particle.connected()) {
    int nmins = Time.hour() * 60 + Time.minute();
    int isave = -1;
    for(int ii = 0; ii < 4; ii++) {
      if ((tidearr[ii].imins > nmins) && tidearr[ii].type == 'L') {
        isave = ii;
        break;
      }
    }
    if (isave >= 0) {
      int hours = tidearr[isave].imins / 60;
      int mins = tidearr[isave].imins - 60 * hours;
      return snprintf(Ltidebuf, 7, "%02d%02d%02d", 0, mins, hours);  
    } else {
      return snprintf(Ltidebuf, 7, "::::::");
    }
  } else {
    return snprintf(Ltidebuf, 7, "::::::");
  }
}

int writetemp() {
  //return snprintf(tempbuf, 7, "%02d%02d%02d", temp, 0, hum);
  return snprintf(tempbuf, 7, "%02d%02d%02d", hum % 100, (spidelta / 10000) % 100, temp % 100);  
}

void packbuf(const char *ds) {
  memset(spibuf, 0, 4);

  for (int i = 0; i < 6; ++i) {
    spibuf[i / 2] |= ((ds[i] - '0') << ((i % 2) ? 4 : 0));
  }

  // in rev A board, lower left colon is 0x01, upper left 0x02, lower right is 0x04, upper right is 0x08
  //if (the_state == STATE_DISPLAY_TIME) {
    spibuf[3] = colons; //0x0F;  
  //} else {
  //  spibuf[3] = 0;
  //}

  //flash colons until cloud connected

  if (!Particle.connected() && ( (ds[1] % 2) == 0)) {
    spibuf[3] = 0;
  }

  // for flashing colons .. not needed with seconds tubes changing
  //if (ds[1] % 2) {
  //  spibuf[3] = 0;
  //} else {
  //  spibuf[3] = 255;
  //}
}

void spi_send_finish() {
  digitalWrite(D6, HIGH);
  digitalWrite(D6, LOW);
}

unsigned slot_minor_counter = 0;
unsigned slot_major_counter = 0;
unsigned date_minor_counter = 0;
unsigned temp_minor_counter = 0;
unsigned tide_minor_counter = 0;

void enter_slot_machine() {
  
  // this happens once a minute, let the tide request ride along here

  if (Particle.connected()) { //if connected see if we have to get new tide data
    time_t tnow = Time.now();
    String i86time = Time.format(tnow, TIME_FORMAT_ISO8601_FULL);
    String day = i86time.substring(0,10);
    Serial.printlnf("day, tideDay %s %s", day.c_str(), tideDay.c_str());
    if (tideDay != day) {
      String data = String(10);
      Particle.publish("hilo", data, PRIVATE);
      Serial.printlnf("did Particle.publish") ;
    }
  }

  the_state = STATE_SLOT_MACHINE;
  slot_major_counter = slot_minor_counter = 0;
}

void enter_date_display() {
  the_state = STATE_DISPLAY_DATE;
  date_minor_counter = 0;
}

void enter_temp_display() {
  the_state = STATE_DISPLAY_TEMP;
  temp_minor_counter = 0;
}

void enter_htide_display() {
  the_state = STATE_HIGH_TIDE;
  tide_minor_counter = 0;
}

void enter_ltide_display() {
  the_state = STATE_LOW_TIDE;
  tide_minor_counter = 0;
}

void do_display_time() {
  colons = 0xF;
  writetime(); 
  
  if ((timebuf[0] == '0') && (timebuf[1] == '1'))
  {
    //temp = (9.0/5.0) * Si7021.readTemperature() + 32.0;
    //hum = Si7021.readHumidity();

    if (nonVol.dateDisp) {
      enter_date_display();
    } else {
      enter_slot_machine();
    }
    return; // don't display the :01 time
  }
  
  char spilast[4];
  
  if (timebuf[1] != lastTime) { //true once a second
    char tb3 = timebuf[3];
    char tb1 = timebuf[1];
    if (nonVol.countback && (timebuf[1] == '0') && (lastTime == '9')) {
      for(int j=9; j >= 0; j--) {  
        timebuf[1] = '0' + j;     //secs digit
        if ((tb3 == '0') && (tb1 == '0') && (timebuflast[3] == '9')) {
          timebuf[3] = '0' + j;   //mins digit
        }
        packbuf(timebuf);
        SPI.transfer(spibuf, NULL, 4, spi_send_finish);
        //Serial.println(timebuf);
        delay(30);
      }
    }
  
    // use packbuf to store the 4-byte SPI string for the last time, save it
    // in spilast[]

    packbuf(timebuflast);
    for (int i=0; i < 4; i++) {
      spilast[i] = spibuf[i];
    }

    char spidim[4];
    char spidimlast[4];
    char timebufdim[7];
    char timebuflastdim[7];

    strcpy(timebufdim, timebuf);
    timebufdim[1] = ':';
    packbuf(timebufdim); 
    for (int i=0; i< 4; i++) {
      spidim[i] = spibuf[i];
    }

    strcpy(timebuflastdim, timebuflast);
    timebuflastdim[1] = ':';
    packbuf(timebuflastdim);  
    for (int i=0; i < 4; i++){
      spidimlast[i] = spibuf[i];
    }
    
    // restore spibuf for current (new) time

    packbuf(timebuf);
    
    // outer loop with FADELEVELS * FADEMULT controls how long the fade takes
    // currently around 200msec .. this seems like a good compromise
    // the fade steps thru FADELEVELS of "pseudo PWM" mixing the old and new timed
    // during the total time of the outer loop.

    #define FADELEVELS 12
    #define FADEMULT 12

    // limit FADELEVELS * FADEMULT to < 300 (ish to keep the outer loop below 1s)

    spidelta = 0;

    if (nonVol.fadeDim) {
      spistart = micros();
      for (int i=0; i < FADELEVELS * FADEMULT; i++){
        for(int k=0; k < FADELEVELS; k++){
          if ((k * FADEMULT) < i){
            SPI.transfer(spidimlast, NULL, 4, spi_send_finish);           
          } else {
            SPI.transfer(spilast, NULL, 4, spi_send_finish);           
          }
        }
      }
      
      for (int i=0; i < FADELEVELS * FADEMULT; i++){
        for(int k=0; k < FADELEVELS; k++){
          if ((k * FADEMULT) < i) {
            SPI.transfer(spibuf, NULL, 4, spi_send_finish);           
          } else {
            SPI.transfer(spidim, NULL, 4, spi_send_finish);           
          }
        }
      }
      spiend = micros();
      spidelta = (int)(spiend - spistart);
    } 
    
    if (nonVol.fadeDigits) {
      for (int i=0; i < FADELEVELS * FADEMULT; i++){
        for(int k=0; k < FADELEVELS; k++){
          if (k * FADEMULT < i){
            SPI.transfer(spibuf, NULL, 4, spi_send_finish);           
          } else {
            SPI.transfer(spilast, NULL, 4, spi_send_finish);           
          }
        }
      }
      spiend = micros();
      spidelta = (int)(spiend - spistart);
    }

    SPI.transfer(spibuf, NULL, 4, spi_send_finish);
    Serial.printlnf("Time %s Packed %x %x %x Looptime %.1f spidelta %d %d", timebuf, spibuf[0], spibuf[1], spibuf[2], loopTime, spidelta, (spidelta/10000) % 100);
    lastTime = timebuf[1];
    for (int i=0; i < 6; i++){
      timebuflast[i] = timebuf[i]; 
    }
  }
}

void do_slot_machine() {
  colons = 0;
  // synch to system/cloud time each minute when the slot machine starts
  if (Time.isValid() &&  rtcAvailable && (slot_minor_counter == 0) && (slot_major_counter == 0) ) {
    unixTime = Time.now();
    rtc.adjust(unixTime + nonVol.offsetFromGMT * 60 * 60); //Time zone adj goes here
    Serial.printlnf("Doing rtc.adjust");    
    rtcValid = true;
  }
  memset(timebuf, '0' + (char)slot_minor_counter, 6);
  packbuf(timebuf);
  SPI.transfer(spibuf, NULL, 4, spi_send_finish);

  ++slot_minor_counter;
  if (10 == slot_minor_counter) {
    slot_minor_counter = 0;
    ++slot_major_counter;
    Serial.printlnf("Slot %d", slot_major_counter);
  }
  
  if (3 < slot_major_counter && (rtcValid || Time.isValid())) {    
    lastTime = -1;
    the_state = STATE_DISPLAY_TIME;
  } else {
    delay(80);
  }
}

void do_display_date() {
  colons = 0;
  if (date_minor_counter == 0) {
    if (rtcValid) {
      now = rtc.now();        
      Serial.printlnf("%02d/%02d/%02d", now.month(), now.day(), now.year() % 100);    
    }
  }
  writedate();
  packbuf(datebuf);
  SPI.transfer(spibuf, NULL, 4, spi_send_finish);

  ++date_minor_counter;
  if (date_minor_counter >= 40) {
    if (nonVol.tideDisp) {
      enter_htide_display();
    } else {
      enter_slot_machine();
    }
    //enter_temp_display();
  }
  delay(80);
}

//only for debugging if a temp display module included
//comment out enter_slot_machine() and uncomment enter_temp_display() in do_display_date()

void do_display_temp() {
  colons = 0;

  if (temp_minor_counter == 0) {
    Serial.printlnf("Temp %d Humid %d", temp, hum);
  }
  writetemp();
  packbuf(tempbuf);
  SPI.transfer(spibuf, NULL, 4, spi_send_finish);

  ++temp_minor_counter;
  if (temp_minor_counter >= 40) {
    enter_slot_machine();
  }
  delay(80);
}

void do_display_high_tide() {
  writeHtide();
  if (Htidebuf[0] == ':') { // skip if not another high tide
    enter_ltide_display();
    return;
  }  
  colons = 0x08 + 0x02; //upper colons
  if (tide_minor_counter == 0) {
    Serial.printlnf("Tide %s", Htidebuf);
  }
  packbuf(Htidebuf);
  SPI.transfer(spibuf, NULL, 4, spi_send_finish);

  ++tide_minor_counter;
  if (tide_minor_counter >= 40) {
    enter_ltide_display();
  }
  delay(80);
}

void do_display_low_tide() {
  writeLtide();
  if (Ltidebuf[0] == ':') {
    enter_slot_machine();
    return;
  }
  colons = 0x04 + 0x01; //lower colonsif (tide_minor_counter == 0) {
    Serial.printlnf("Tide %s", Ltidebuf);
  
  packbuf(Ltidebuf);
  SPI.transfer(spibuf, NULL, 4, spi_send_finish);

  ++tide_minor_counter;
  if (tide_minor_counter >= 40) {
    enter_slot_machine();
  }
  delay(80);
}

void checkEnc() {
  int newPosition = Enc.read();
  if (newPosition != oldPosition) {
    dutyCycle = dutyCycle + 5 * (newPosition - oldPosition);
    oldPosition = newPosition;
    if (dutyCycle > 255) {
      dutyCycle = 255;
    }
    if (dutyCycle < 0) {
      dutyCycle = 0;
    }
    Serial.printlnf("Setting Duty Cycle to %d", dutyCycle);
    analogWrite(A2, dutyCycle, PWMFREQ);
  }  
}

bool published = false;

// loop() runs over and over again, as quickly as it can execute.
void loop() {
  if (startup) {
    analogWrite(A2, dutyCycle, PWMFREQ);
    startup = false;
  }
  switch (the_state) {
    case STATE_DISPLAY_TIME: do_display_time();       break;
    case STATE_DISPLAY_DATE: do_display_date();       break;
    case STATE_SLOT_MACHINE: do_slot_machine();       break;
    case STATE_DISPLAY_TEMP: do_display_temp();       break;    
    case STATE_HIGH_TIDE:    do_display_high_tide();  break;
    case STATE_LOW_TIDE:     do_display_low_tide();   break;    
  }
  checkEnc();

  if (the_state == STATE_DISPLAY_TIME) {
    float dt = (float)(micros() - lastmicro);
    if (lastLoop < 0.0) {
      loopTime = dt;
    } else {
      loopTime = dt;
    }
    lastLoop = dt;
  } else {
    lastLoop = -1.0;
  }
  lastmicro = micros();
}
