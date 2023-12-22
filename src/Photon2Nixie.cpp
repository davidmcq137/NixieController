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

typedef enum {
  STATE_DISPLAY_TIME,
  STATE_SLOT_MACHINE,
  STATE_DISPLAY_DATE,
  STATE_DISPLAY_TEMP
} my_state_t;

my_state_t the_state;

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
  Time.zone(-5);

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

  Si7021.begin();  
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
  return snprintf(tempbuf, 7, "%02d%02d%02d", hum % 100, 00, temp % 100);  
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
  if ((timebuf[0] == '0') && (timebuf[1] == '1'))
  {
    temp = (9.0/5.0) * Si7021.readTemperature() + 32.0;
    hum = Si7021.readHumidity();
    enter_date_display();
    return; // don't display the :01 time
  }

  writetime(); 
  
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
    
    #define DIGITFADE ON
    #ifdef DIGITFADE
    
    // outloop with FADELEVELS * FADEMULT controls how long the fade takes
    // currently around 200msec .. this seems like a good compromise
    // the fade steps thru FADELEVELS of "pseudo PWM" mixing the old and new time

    #define FADELEVELS 20
    #define FADEMULT 10

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
    #endif
  
    SPI.transfer(spibuf, NULL, 4, spi_send_finish);
    Serial.printlnf("Time %s Packed %x %x %x Looptime %.1f spidelta %d", timebuf, spibuf[0], spibuf[1], spibuf[2], loopTime, spidelta);
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
    rtc.adjust(unixTime - 5*60*60); //Time zone adj goes here
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
  //if (3 < slot_major_counter && Time.isValid()) {
  //Serial.printlnf("maj, rtcValid, Time.isValid() %d %d %d", slot_major_counter, rtcValid, Time.isValid());
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
      Serial.println("Foo");
    } else {
      loopTime = dt;
    }
    lastLoop = dt;
  } else {
    lastLoop = -1.0;
  }
  lastmicro = micros();
}
