#include "LowPower.h"

int SER_Pin = 4;   //pin 14 on the 75HC595
int RCLK_Pin = 7;  //pin 12 on the 75HC595
int SRCLK_Pin = 6; //pin 11 on the 75HC595
int S_CL_Pin = 5;
int OE_Pin = 8;

#define SENSOR_POWER 14 // pin 12 on a pro mini
#define SENSOR_READ A2
#define SENSOR_BOOT 1000

#define MINUTE 60000L
#define SECOND 1000L 

#define BAR_GRAPH_TIME 4000

#define MANUAL_WATER_TIME 15000

#define SOLENOID_PULSE_WIDTH 1000

#define water_on_thresh 380
//#define water_on_thresh 600
#define water_off_thresh 400

#define PSU_EN 9

#define P1G 2
#define P2G 3

#define N1G 15 // pin 13 on a pro mini
#define N2G 16 // pin 11 on a pro mini

#define S1_LED A3
#define S2_LED A1
#define S1_VAL A0
#define S2_VAL 10

//How many of the shift registers - change this
#define number_of_74hc595s 2 

//do not touch
#define numOfRegisterPins number_of_74hc595s * 8

// Comment out to enable delay() instead of powerDown()
#define LOW_POWER

// removes calls to Serial
//#define DEBUG

void manualWater();
void displayBattLevel();
void displayMoistureLevel();

// for button debounce... not really used as we only wake up so often, and don't use interrupts.
static unsigned long last_interrupt_time = 0;

// number of millis in a day
unsigned long day_millis = 0;

// number of millis spent watering today
unsigned long water_today = 0;

// max amount of millis watering per day
unsigned long max_water_daily = 0;

// min amount of millis watering per day
unsigned long min_water_daily = 0;

// millis() when the current day (24 hour period) will end
unsigned long day_end = 0;

// when today started
unsigned long day_start = 0;

// how often to wake to check the moisture level
unsigned long wake_interval = 0;

// how long to water for
unsigned long watering_stretches = 0;

// how long to wait in between watering streteches
unsigned long watering_breaks = 0;

// day counter
int days_on = 0;

// last moisture sensor value
int last_sensor = 0;

// current latching solenoid state
boolean solenoidOpen = false;

// state of bargraph segments
boolean registers[numOfRegisterPins];

/**
 * Responds to the current button state
 */
void checkButtons() {
  int b1 = digitalRead(S1_VAL);
  int b2 = digitalRead(S2_VAL);
  
  
  if (b1 == HIGH && b2 == HIGH) {
#ifdef DEBUG
    Serial.println("both pressed");
#endif
    manualWater();
  } else if (b1 == HIGH) {
#ifdef DEBUG
    Serial.println("b1");
#endif
    displayBattLevel();
  } else if (b2 == HIGH) {
#ifdef DEBUG
    Serial.println("b2");
#endif
    displayMoistureLevel();
  }
}

/**
 * Sleeps for the given amount of time while periodically checking the button state
 */
void sleepDelay(unsigned long amount) {
#ifdef LOW_POWER
  int sleeps = amount / 8000; // max sleep time is 8s, so this is the number of times we'll have to sleep

  while (sleeps-- > 0) {
    checkButtons();
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
  }
#else
  for (int i=0; i< amount / 100; i++) {
    checkButtons();
    delay(amount / 100);
  }
#endif
}

/**
 * Resets the h-bridge to its default state
 */
void resetBridge() {
  // let the pull up/downs take care of this to start  
  pinMode(P1G, INPUT);
  pinMode(P2G, INPUT);
  pinMode(N1G, INPUT);
  pinMode(N2G, INPUT);
}

/**
 * Sets the solenoid to the requested state, enabling the requested P and N gates. Verifies that the request won't cause a short
 */
void toggleSolenoid(int p, int n) {
  if ((p == P1G && n == N1G) || (p == P2G && n == N2G)) {
    Serial.println("Short circuit warning.");
    return;
  }

  resetBridge();
  
  delay(200);
  
  pinMode(p, OUTPUT);
  delay(50);
  digitalWrite(p, LOW);
  delay(50);
  pinMode(n, OUTPUT);
  delay(50);
  digitalWrite(n, HIGH);
  
  delay(SOLENOID_PULSE_WIDTH);
  
  resetBridge();
}

void openSolenoid() {
  toggleSolenoid(P2G, N1G);
  solenoidOpen = true;
}

void closeSolenoid() {
  toggleSolenoid(P1G, N2G);
  solenoidOpen = false;
}

void setup(){
  resetBridge();
  delay(10);
  closeSolenoid();
  
  pinMode(SER_Pin, OUTPUT);
  pinMode(RCLK_Pin, OUTPUT);
  pinMode(SRCLK_Pin, OUTPUT);
  pinMode(OE_Pin, OUTPUT);
  pinMode(S_CL_Pin, OUTPUT);
  
  pinMode(S1_LED, OUTPUT);
  pinMode(S1_VAL, INPUT);

  pinMode(S2_LED, OUTPUT);
  pinMode(S2_VAL, INPUT);
  
  pinMode(SENSOR_POWER, OUTPUT);
  
  pinMode(PSU_EN, INPUT);
  
  digitalWrite(S1_LED, LOW);
  digitalWrite(S2_LED, LOW);
  
  delay(10);
  digitalWrite(OE_Pin, LOW);
  digitalWrite(S_CL_Pin, HIGH);

  day_millis = MINUTE * 60L * 24L;
  water_today = 0;
  max_water_daily = MINUTE * 15L; // no more than 15 minutes of water per day
  min_water_daily = MINUTE; // no fewer than 1 minute of water per day

  day_end = 0; // millis at day end
  day_start = 0;
  wake_interval = MINUTE * 60; // check every 60 minutes
  watering_stretches = MINUTE * 3 / 4; // water for 45s
  watering_breaks = MINUTE * 5L; // wait for 5 minutes in between watering

  day_end = millis() + day_millis;

#ifdef DEBUG
  Serial.begin(38400);
#endif

  //reset all led register pins
  clearRegisters();
  writeRegisters();
}               

//set all register pins to LOW
void clearRegisters(){
  for(int i = numOfRegisterPins - 1; i >=  0; i--){
     registers[i] = LOW;
  }
} 

/**
 * Checks the moisture sensor, averaging over the number of requested samples, waiting the requested amount of time in between
 */
int check_moisture(int times, int delays) {
  digitalWrite(SENSOR_POWER, HIGH);
  delay(SENSOR_BOOT);

  int final = 0;
  
  for (int i=0; i<times; i++) {
    final += analogRead(SENSOR_READ);
    
    if (i+1 <times)
      delay(delays);
  }
  
  digitalWrite(SENSOR_POWER, LOW);
  last_sensor = final / times;
  return last_sensor;
}

/**
 * Determines whether the current moisture level exceeds the requested threshold
 */
boolean check_and_report_for_thresh(int thresh) {
  int moisture = check_moisture(2, 10);
  
  #ifdef DEBUG
  Serial.print("Moisture level ");
  Serial.print(moisture);
  #endif
  if (moisture < thresh) {
    #ifdef DEBUG
    Serial.println(", watering");
    #endif
    return true;
  } else
    #ifdef DEBUG
    Serial.println(", not watering");
    #endif
    
  return false;
}

/**
 * Enables the solenoid according to the timing parameters until the moisture level reaches target
 */
void water() {
  boolean needs_water = true;
  unsigned long start_millis = 0;
  
  while (needs_water && (water_today < max_water_daily)) {
    start_millis = millis();
    openSolenoid();
  
    #ifdef DEBUG
    Serial.print("Solenoid on. Sleeping for ");
    Serial.println(watering_stretches);
    #endif
    
    sleepDelay(watering_stretches);
    if (millis() < start_millis)
      water_today += millis(); // yes, this is less, but it's a f*cking garden.
    else
      water_today += (millis() - start_millis);
    
    #ifdef DEBUG
    Serial.print("Solenoid off. Sleeping for ");
    Serial.println(watering_breaks);
    #endif

    closeSolenoid();
    sleepDelay(watering_breaks);  
    
    needs_water = check_and_report_for_thresh(water_off_thresh);

    #ifdef DEBUG
    Serial.print("watered for ");
    Serial.print(water_today / 60000L);
    Serial.print(" minutes today, ");
    Serial.print((max_water_daily - water_today) / 60000L);
    Serial.println(" max remaining");
    #endif
  }
  
  if (solenoidOpen)
    closeSolenoid();
}

/**
 * Set and display registers
 * Only call AFTER all values are set how you would like (slow otherwise)
 */
void writeRegisters(){

  digitalWrite(RCLK_Pin, LOW);

  for(int i = numOfRegisterPins - 1; i >=  0; i--){
    digitalWrite(SRCLK_Pin, LOW);

    int val = registers[i];

    digitalWrite(SER_Pin, val);
    digitalWrite(SRCLK_Pin, HIGH);

  }
  
  digitalWrite(RCLK_Pin, HIGH);
}

/**
 * Set an individual pin HIGH or LOW
 */
void setRegisterPin(int index, int value){
  registers[index] = value;
}

/**
 * Blinks the led on a button (pin) 
 */
void blink(int pin) {
  for (int i=0; i<3; i++) {
    digitalWrite(pin, HIGH);
    delay(100);
    digitalWrite(pin, LOW);
    delay(100);
  }
}

/**
 * Displays a value (0-9) on the led bargraph. Values > 9 will be truncated to 9
 */
void displayValue(int value) {
  int v = value > 9 ? 9 : value;
  
  clearRegisters();
  for (int i=0; i<v; i++)
    setRegisterPin(i, HIGH);
    
  writeRegisters();
  delay(BAR_GRAPH_TIME);
  clearRegisters();
  writeRegisters();
}

/**
 * Enables the solenoid for MANUAL_WATER_TIME
 */
void manualWater() {
  // blink both
  digitalWrite(S1_LED, HIGH);
  digitalWrite(S2_LED, HIGH);
  delay(1000);
  digitalWrite(S1_LED, LOW);
  digitalWrite(S2_LED, LOW);
  
  openSolenoid();
  sleepDelay(MANUAL_WATER_TIME);
  closeSolenoid();
}

/**
 * Displays the battery level on the bargraph
 */
void displayBattLevel() {

}

/**
 * Displays the moisture level on the bargraph
 */
void displayMoistureLevel() {
  int level = check_moisture(2, 10) / 102;
  
  displayValue(level);
}

void loop(){

  checkButtons();

  if (water_today < max_water_daily && check_and_report_for_thresh(water_on_thresh))
      water();
  
  #ifdef DEBUG
  Serial.println("Loop done. Sleeping");
  #endif
  

  // first case - no overflow, millis needs to pass day end
  // second case - overflow. millis needs to be less than start (overflow) and greater than end (still pass it)
  #ifdef DEBUG
  Serial.println(day_start);
  Serial.println(day_end);
  Serial.println(millis());
  #endif
  if (((day_end > day_start) && (day_end < millis())) ||
      ((day_end < day_start) && (day_end < millis()) && (day_start > millis()))) {
        
      if (water_today < min_water_daily) {
        #ifdef DEBUG
        Serial.print("Water deficiency. Watering for another ");
        Serial.println(min_water_daily - water_today);
        #endif
        
        openSolenoid();
        sleepDelay(min_water_daily - water_today); 
        closeSolenoid();
      }
        
      day_start = millis();
      day_end = millis() + day_millis;
      water_today = 0;
      days_on++;

      #ifdef DEBUG
      Serial.print("New day. Days on ");
      Serial.println(days_on);
      #endif
  }
  
  sleepDelay(wake_interval);
}

