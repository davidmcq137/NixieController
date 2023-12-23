/* 
 * Project myProject
 * Author: Your Name
 * Date: 
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/firmware/best-practices/firmware-template/
 */

// Include Particle Device OS APIs
#include "Particle.h"
#include "Encoder.h"

#include "Adafruit_Si7021.h"
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
char spibuf[4];
int lastTime;


#define ONE_DAY_MILLIS (24 * 60 * 60 * 1000)
#define DS3231Addr 0x68
#define PWMFREQ (200)

unsigned long lastSync = millis();
RTC_DS3231 rtc;
Adafruit_Si7021 Si7021;
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
bool fadeDigits = true;
int offsetFromGMT = -5;

typedef enum {
  STATE_DISPLAY_TIME,
  STATE_SLOT_MACHINE,
  STATE_DISPLAY_DATE,
  STATE_DISPLAY_TEMP
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
  analogWrite(A2, dutyCycle, PWMFREQ);

  //Still need this even with RTC since particle OS won't give Time.isValid() until TZ set
  Time.zone(offsetFromGMT);

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
  Si7021.begin();  

  // Start the Bluetooth service and begin advertising

  BLE.setDeviceName("Nixie Clock");
  BLE.on();
  BLE.addCharacteristic(txCharacteristic);
  BLE.addCharacteristic(rxCharacteristic);
  BleAdvertisingData data;
  data.appendServiceUUID(serviceUuid);
  BLE.advertise(&data);
  
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

void sendSPI(String str, float val)
{
  {
    size_t nch;
    char buf[20];
    uint8_t txbuf[20];

    nch = sprintf(buf, "(" + str + ":%4.2f)", val);
    for (size_t ii = 0; ii < nch; ii++)
    {
      txbuf[ii] = (int)buf[ii];
    }
    txCharacteristic.setValue(txbuf, nch);
  }
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
    Serial.printlnf("PWM command %d", (int)setPWM);
    analogWrite(A2, (int)setPWM, PWMFREQ);
  } else if (k == "Fade") {
    if (v == "ON") {
      fadeDigits = true;
      sendSPI("Fade ON", 0);
    } else {
      sendSPI("Fade OFF", 0);
      fadeDigits = false;
    }
  } else if (k == "ssid")
  {
    wifissid.concat(v);
  }
  else if (k == "pwd")
  {
    wifipwd.concat(v);
  }
  else if (k == "update")
  {
    WiFi.clearCredentials();
    WiFi.setCredentials(wifissid, wifipwd);
    WiFi.on();
    WiFi.connect();

    int wLoops = 0;
    while (!WiFi.ready() && wLoops < 300){ //timeout 30 seconds if no wifi or bad creds
      delay(100);
      wLoops = wLoops + 1;
    }
    if (wLoops >= 300) {
      sendSPI("No WiFi Conn", -1); //No WiFi connection
      return;
    }
    sendSPI("WiFi connected", 2); // WiFi connected
    Particle.connect();
    wLoops = 0;
    while (!Particle.connected() && wLoops < 300)
    {
      delay(100);
      wLoops = wLoops + 1;
    }
    if (wLoops >= 300) {
      sendSPI("No Cloud Conn", -30); // No Particle Cloud Connection 
      return;
    }
    sendSPI("Cloud Connected", 30); // particle cloud connected
  }
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

int writetemp() {
  //return snprintf(tempbuf, 7, "%02d%02d%02d", temp, 0, hum);
  return snprintf(tempbuf, 7, "%02d%02d%02d", hum % 100, (spidelta / 10000) % 100, temp % 100);  
}

void packbuf(const char *ds) {
  memset(spibuf, 0, 4);

  for (int i = 0; i < 6; ++i) {
    spibuf[i / 2] |= ((ds[i] - '0') << ((i % 2) ? 4 : 0));
  }

  if (the_state == STATE_DISPLAY_TIME) {
    spibuf[3] = 255;  
  } else {
    spibuf[3] = 0;
  }

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

void enter_slot_machine() {
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

void do_display_time() {
  writetime(); 
  
  if ((timebuf[0] == '0') && (timebuf[1] == '1'))
  {
    Serial.println("timebuf 0/1");
    temp = (9.0/5.0) * Si7021.readTemperature() + 32.0;
    hum = Si7021.readHumidity();
    enter_date_display();
    return; // don't display the :01 time
  }
  
  char spilast[4];

  if (timebuf[1] != lastTime) {
  
    // use packbuf to store the 4-byte SPI string for the last time, save it
    // in spilast[]

    packbuf(timebuflast);
    for (int i=0; i < 4; i++) {
      spilast[i] = spibuf[i];
    }
    
    // restore spibuf for current (new) time

    packbuf(timebuf);
    
    // outer loop with FADELEVELS * FADEMULT controls how long the fade takes
    // currently around 200msec .. this seems like a good compromise
    // the fade steps thru FADELEVELS of "pseudo PWM" mixing the old and new timed
    // during the total time of the outer loop.

    #define FADELEVELS 16
    #define FADEMULT 8

    // limit FADELEVELS * FADEMULT to < 300 (ish to keep the outer loop below 1s)

    if (fadeDigits) {
      spistart = micros();
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
    } else {
      spidelta = 0;
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

  // synch to system/cloud time each minute when the slot machine starts
  if (Time.isValid() &&  rtcAvailable && (slot_minor_counter == 0) && (slot_major_counter == 0) ) {
    unixTime = Time.now();
    rtc.adjust(unixTime + offsetFromGMT * 60 * 60); //Time zone adj goes here
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
    //enter_slot_machine();
    enter_temp_display();
  }
  delay(80);
}

void do_display_temp() {
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

// loop() runs over and over again, as quickly as it can execute.
void loop() {
  switch (the_state) {
    case STATE_DISPLAY_TIME: do_display_time(); break;
    case STATE_DISPLAY_DATE: do_display_date(); break;
    case STATE_SLOT_MACHINE: do_slot_machine(); break;
    case STATE_DISPLAY_TEMP: do_display_temp(); break;    
  }
  checkEnc();

  if (the_state == STATE_DISPLAY_TIME) {
    float dt = (float)(micros() - lastmicro);
    if (lastLoop < 0.0) {
      loopTime = dt;
      //Serial.println("Foo");
    } else {
      loopTime = dt;
    }
    lastLoop = dt;
  } else {
    lastLoop = -1.0;
  }
  lastmicro = micros();
}
