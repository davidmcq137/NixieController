/* 
 * Project myProject
 * Author: Your Name
 * Date: 
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/firmware/best-practices/firmware-template/
 */

// Include Particle Device OS APIs
#include "Particle.h"
#include "RTClibrary.h"

// Let Device OS manage the connection to the Particle Cloud
SYSTEM_MODE(AUTOMATIC);

// Run the application and system concurrently in separate threads
SYSTEM_THREAD(ENABLED);

// Show system, cloud connectivity, and application logs over USB
// View logs with CLI using 'particle serial monitor --follow'
SerialLogHandler logHandler(LOG_LEVEL_INFO);

unsigned counter;
char timebuf[7];
char datebuf[7];
char spibuf[4];
int lastTime;


#define ONE_DAY_MILLIS (24 * 60 * 60 * 1000)
#define DS3231Addr 0x68;

unsigned long lastSync = millis();
RTC_DS3231 rtc;
time32_t unixTime;
DateTime now;
bool rtcValid;
bool rtcAvailable;

typedef enum {
  STATE_DISPLAY_TIME,
  STATE_SLOT_MACHINE,
  STATE_DISPLAY_DATE
} my_state_t;

my_state_t the_state;

// setup() runs once, when the device is first turned on
void setup() {
  Serial.begin();
  waitFor(Serial.isConnected, 1000);


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
  pinMode(D7, OUTPUT);
  analogWrite(D7, 255, 200);

  //Still need this even with RTC since particle OS won't give Time.isValid() until TZ set
  Time.zone(-5);
  //Serial.println("Time zone set to -5 from UTC");

  // rtc.begin() always returns true, even if no rtc device. heaven knows why
  // so check manually by talking directly to the i2c bus if it's there  
  // at the expected address

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
  Wire.end();

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

void enter_slot_machine() {
  the_state = STATE_SLOT_MACHINE;
  slot_major_counter = slot_minor_counter = 0;
}

void enter_date_display() {
  the_state = STATE_DISPLAY_DATE;
  date_minor_counter = 0;
}

void do_display_time() {
  writetime(); 

  if (timebuf[1] != lastTime) {
    packbuf(timebuf);
    Serial.printlnf("Time %s Packed %x %x %x", timebuf, spibuf[0], spibuf[1], spibuf[2]);
    SPI.transfer(spibuf, NULL, 4, spi_send_finish);
    lastTime = timebuf[1];
  }
  
  if ((timebuf[0] == '0') && (timebuf[1] == '1'))
  {
    enter_date_display();
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
  writedate();
  packbuf(datebuf);
  SPI.transfer(spibuf, NULL, 4, spi_send_finish);

  ++date_minor_counter;
  if (date_minor_counter >= 40) {
    enter_slot_machine();
  }
  delay(80);
}

// loop() runs over and over again, as quickly as it can execute.
void loop() {
  switch (the_state) {
    case STATE_DISPLAY_TIME: do_display_time(); break;
    case STATE_DISPLAY_DATE: do_display_date(); break;
    case STATE_SLOT_MACHINE: do_slot_machine(); break;

  }
}
