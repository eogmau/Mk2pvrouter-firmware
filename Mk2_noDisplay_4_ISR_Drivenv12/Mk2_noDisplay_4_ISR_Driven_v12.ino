/* Mk2_noDisplay_4_ISR_Driven_v12.ino
 *
 * This sketch is for diverting suplus PV power to a dump load.
 * It is based on the Mk2i PV Router code by Robin Emley.
 *
 * This version has been significantly re-architected to be ISR-driven.
 * - All time-critical processing now occurs within the Timer1 ISR.
 * - Settings are stored in EEPROM and configurable via the serial monitor.
 * - NEW: Automatic detection of a full hot water tank by monitoring power flows using the CT2.
 * - NEW: Automatic periodic re-testing of the load to resume heating.
 * - NEW: Watchdog Timer for fail-safe operation.
 * - NEW: Highly efficient, numeric-only IoT serial logging format.
 * - FIX: Corrected timing logic for export status calculation to prevent flicker.
 *
 * ---------------------------------------------------------------------
 * -- SERIAL COMMANDS --
 * HELP                 - Displays this list of commands.
 * STATUS               - Shows the current settings loaded in memory.
 * SAVE                 - Saves the current settings to EEPROM.
 * LOAD                 - Reloads the last saved settings from EEPROM.
 * RESET                - Resets all settings to factory defaults.
 * SET <param> <value>  - Changes a parameter (e.g., "SET PCG 0.045").
 * ENABLE / DISABLE     - Controls overall diverter operation.
 * FULL_LOAD_ON / OFF   - Controls the "boost" mode.
 * ---------------------------------------------------------------------
 *
 */

#include <Arduino.h> 
#include <TimerOne.h>
#include <EEPROM.h>
#include <avr/wdt.h> // For the Watchdog Timer

// ---------- System Constants ----------
#define ADC_TIMER_PERIOD 125 // uS (determines the sampling rate)
#define CYCLES_PER_SECOND 50 
#define JOULES_PER_WATT_HOUR 3600
#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define ACCUMULATOR_RESET_IN_HOURS 8
#define CONTINUITY_CHECK_MAXCOUNT 250 // mains cycles for logging
#define PERSISTENCE_FOR_POLARITY_CHANGE 1 // Number of samples to confirm polarity change
#define TANK_FULL_EXPORT_THRESHOLD_WATTS 50 // Watts of export to confirm tank is full
#define TANK_FULL_DETECTION_CYCLES 250 // How many cycles (5s) to confirm tank is full
#define TANK_RETEST_INTERVAL_SECONDS 15 // Time before re-testing a full tank

// ---------- EEPROM Settings Structure ----------
struct Settings {
  long magic_value;
  float powerCal_grid;
  float powerCal_diverted;
  long working_range_joules;
  long required_export_watts;
  long anti_creep_limit;
  long export_while_diverting_threshold;
  bool debugMode;
};
Settings settings;
const long MAGIC_VALUE = 123456789L;

// ---------- Global Volatile Variables (for ISR/main loop communication) ----------
volatile boolean diverterEnabled = true;
volatile boolean fullLoadActive = false;
volatile boolean tankIsFull = false;
volatile boolean isExporting = false;
volatile boolean wasLoadActiveInLogPeriod = false;
volatile boolean wasExportingInLogPeriod = false;
volatile enum loadStates {LOAD_ON, LOAD_OFF} nextStateOfLoad = LOAD_OFF;
volatile enum polarities {NEGATIVE, POSITIVE} polarityConfirmed;
volatile enum outputModes {ANTI_FLICKER, NORMAL} outputMode;
volatile unsigned int divertedEnergyTotal_Wh = 0;
volatile boolean logDataPending = false;

// ---------- Global Variables ----------
long antiCreepLimit_inIEUperMainsCycle, requiredExportPerMainsCycle_inIEU;
long capacityOfEnergyBucket_long, lowerEnergyThreshold_long, upperEnergyThreshold_long, IEU_per_Wh;
const int DCoffset_I = 512;
boolean beyondStartUpPhase = false;
long energyInBucket_long;
long DCoffset_V_long, DCoffset_V_min, DCoffset_V_max;
long divertedEnergyRecent_IEU = 0;
unsigned long accumulatorReset_inMainsCycles, absenceOfDivertedEnergyCount = 0, mainsCyclesPerHour;
float offsetOfEnergyThresholdsInAFmode = 0.1;
enum loadStates loadStateOfPreviousCycle = LOAD_OFF; // NEW: To fix export status timing

// Pin Definitions
const byte outputModeSelectorPin = 3, outputForTrigger = 4, voltageSensor = 3;
const byte currentSensor_diverted = 4, currentSensor_grid = 5;

// =================================================================
//   HELPER FUNCTION DEFINITIONS
// =================================================================

void checkOutputModeSwitch() {
  static int lastPinState = -1;
  int pinState = digitalRead(outputModeSelectorPin);
  if (pinState != lastPinState) {
    lastPinState = pinState;
    outputMode = (enum outputModes)pinState;
    float baseline = ((float)settings.working_range_joules - 1800) / settings.working_range_joules;
    if (outputMode == ANTI_FLICKER) {
      lowerEnergyThreshold_long = capacityOfEnergyBucket_long * (baseline - offsetOfEnergyThresholdsInAFmode); 
      upperEnergyThreshold_long = capacityOfEnergyBucket_long * (baseline + offsetOfEnergyThresholdsInAFmode);   
    } else { 
      lowerEnergyThreshold_long = capacityOfEnergyBucket_long * baseline; 
      upperEnergyThreshold_long = capacityOfEnergyBucket_long * baseline;   
    }
  }
}

void recalculateDerivedParams() {
  capacityOfEnergyBucket_long = (long)settings.working_range_joules * CYCLES_PER_SECOND * (1/settings.powerCal_grid);
  IEU_per_Wh = (long)JOULES_PER_WATT_HOUR * CYCLES_PER_SECOND * (1/settings.powerCal_diverted); 
  antiCreepLimit_inIEUperMainsCycle = (float)settings.anti_creep_limit * (1/settings.powerCal_grid);
  mainsCyclesPerHour = (long)CYCLES_PER_SECOND * SECONDS_PER_MINUTE * MINUTES_PER_HOUR;
  accumulatorReset_inMainsCycles = (long)ACCUMULATOR_RESET_IN_HOURS * mainsCyclesPerHour;                           
  requiredExportPerMainsCycle_inIEU = (long)settings.required_export_watts * (1/settings.powerCal_grid);
  checkOutputModeSwitch();
}

void printSettings() {
  Serial.println("\n--- Current Settings ---");
  Serial.print("Power Cal (Grid): "); Serial.println(settings.powerCal_grid, 4);
  Serial.print("Power Cal (Diverted): "); Serial.println(settings.powerCal_diverted, 4);
  Serial.print("Working Range (Joules): "); Serial.println(settings.working_range_joules);
  Serial.print("Required Export (Watts): "); Serial.println(settings.required_export_watts);
  Serial.print("Anti-Creep Limit (Joules): "); Serial.println(settings.anti_creep_limit);
  Serial.print("Export While Diverting Threshold (W): "); Serial.println(settings.export_while_diverting_threshold);
  Serial.print("Debug Mode (Human-Readable): "); Serial.println(settings.debugMode ? "ON" : "OFF");
  Serial.println("------------------------");
  delay(10);
}

void saveSettings() {
  EEPROM.put(0, settings);
  recalculateDerivedParams();
  Serial.println("Settings saved to EEPROM.");
  printSettings();
}

void resetSettings() {
  settings.magic_value = MAGIC_VALUE;
  settings.powerCal_grid = 0.0459;
  settings.powerCal_diverted = 0.0435;
  settings.working_range_joules = 7200;
  settings.required_export_watts = 0;
  settings.anti_creep_limit = 5;
  settings.export_while_diverting_threshold = 200;
  settings.debugMode = false;
  saveSettings();
}

void loadSettings() {
  EEPROM.get(0, settings);
  if (settings.magic_value != MAGIC_VALUE) {
    Serial.println("EEPROM invalid or empty. Loading factory defaults.");
    resetSettings();
  } else {
    Serial.println("Loaded settings from EEPROM.");
    recalculateDerivedParams();
    printSettings();
  }
}

void handleSerialCommands() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    String command = input;
    String valueStr = "";
    int spaceIndex = input.indexOf(' ');
    if (spaceIndex != -1) {
      command = input.substring(0, spaceIndex);
      valueStr = input.substring(spaceIndex + 1);
    }

    if (command.equalsIgnoreCase("HELP")) {
      Serial.println("\n--- Available Commands ---");
      Serial.println("STATUS, SAVE, LOAD, RESET");
      Serial.println("SET <param> <value> (PCG, PCD, WRJ, REW, ACL, EWT, DEBUG)");
      Serial.println("ENABLE / DISABLE, FULL_LOAD_ON / OFF");
    } else if (command.equalsIgnoreCase("STATUS")) {
      printSettings();
    } else if (command.equalsIgnoreCase("SAVE")) {
      saveSettings();
    } else if (command.equalsIgnoreCase("LOAD")) {
      loadSettings();
    } else if (command.equalsIgnoreCase("RESET")) {
      resetSettings();
    } else if (command.equalsIgnoreCase("SET")) {
      String param = valueStr.substring(0, valueStr.indexOf(' '));
      String value = valueStr.substring(valueStr.indexOf(' ') + 1);
      if (param.equalsIgnoreCase("PCG")) settings.powerCal_grid = value.toFloat();
      else if (param.equalsIgnoreCase("PCD")) settings.powerCal_diverted = value.toFloat();
      else if (param.equalsIgnoreCase("WRJ")) settings.working_range_joules = value.toInt();
      else if (param.equalsIgnoreCase("REW")) settings.required_export_watts = value.toInt();
      else if (param.equalsIgnoreCase("ACL")) settings.anti_creep_limit = value.toInt();
      else if (param.equalsIgnoreCase("EWT")) settings.export_while_diverting_threshold = value.toInt();
      else if (param.equalsIgnoreCase("DEBUG")) settings.debugMode = (value.toInt() == 1);
      else Serial.println("Unknown SET parameter.");
      Serial.println("NOTE: Settings not permanent until 'SAVE'.");
    } else if (command.equalsIgnoreCase("ENABLE")) {
      diverterEnabled = true; Serial.println("OK: Diverter enabled.");
    } else if (command.equalsIgnoreCase("DISABLE")) {
      diverterEnabled = false; fullLoadActive = false; digitalWrite(outputForTrigger, LOAD_OFF);
      Serial.println("OK: Diverter disabled.");
    } else if (command.equalsIgnoreCase("FULL_LOAD_ON")) {
      fullLoadActive = true; digitalWrite(outputForTrigger, LOAD_ON);
      Serial.println("OK: Full Load ON.");
    } else if (command.equalsIgnoreCase("FULL_LOAD_OFF")) {
      fullLoadActive = false; digitalWrite(outputForTrigger, LOAD_OFF);
      Serial.println("OK: Full Load OFF.");
    } else if (command.equalsIgnoreCase("DEBUG_ON")) {
      settings.debugMode = true; Serial.println("OK: Debug Mode ON.");
    } else if (command.equalsIgnoreCase("DEBUG_OFF")) {
      settings.debugMode = false; Serial.println("OK: Debug Mode OFF.");
    } else if (command.length() > 0) {
      Serial.print("Unknown command: "); Serial.println(input);
    }
    delay(10);
  }
}

void logData() {
  if (settings.debugMode) {
    Serial.print("Diverter: "); Serial.print(diverterEnabled ? "ENABLED " : "DISABLED");
    Serial.print("| FullLoad: "); Serial.print(fullLoadActive ? "ON " : "OFF ");
    Serial.print("| Tank: "); Serial.print(tankIsFull ? "FULL(Auto) " : "HEATING ");
    Serial.print("| Exporting: "); Serial.print(wasExportingInLogPeriod ? "YES " : "NO ");
    Serial.print("| Load: "); Serial.print(wasLoadActiveInLogPeriod ? "ON " : "OFF");
    Serial.print("| Diverted (Wh): "); Serial.println(divertedEnergyTotal_Wh);
  } else {
    // Format: DiverterEnabled,FullLoadActive,TankIsFull,IsExporting,LoadIsOn,DivertedEnergy
    Serial.print(diverterEnabled ? "1," : "0,");
    Serial.print(fullLoadActive ? "1," : "0,");
    Serial.print(tankIsFull ? "1," : "0,");
    Serial.print(wasExportingInLogPeriod ? "1," : "0,");
    Serial.print(wasLoadActiveInLogPeriod ? "1," : "0,");
    Serial.println(divertedEnergyTotal_Wh);
  }
  
  wasLoadActiveInLogPeriod = false;
  wasExportingInLogPeriod = false;

  delay(10);
}

void processSample(int sampleV, int sampleI_grid, int sampleI_diverted) {
  static long sumP_grid = 0, sumP_diverted = 0;
  static long cumVdeltasThisCycle_long = 0, lastSampleVminusDC_long = 0;
  static int sampleSetsDuringThisMainsCycle = 0;    
  static enum polarities polarityOfMostRecentVsample, polarityConfirmedOfLastSampleV;
  static unsigned int startup_MainsCycleCount = 0;
  static int sampleCount_forContinuityChecker = 0;
  static int tankFullDetectionCounter = 0;
  static unsigned long tankRetestTimestamp = 0;

  long sampleVminusDC_long = ((long)sampleV << 8) - DCoffset_V_long; 
  polarityOfMostRecentVsample = (sampleVminusDC_long > 0) ? POSITIVE : NEGATIVE;

  static byte persistence_count = 0;
  if (polarityOfMostRecentVsample != polarityConfirmedOfLastSampleV) persistence_count++;
  else persistence_count = 0;
  if (persistence_count > PERSISTENCE_FOR_POLARITY_CHANGE) {
    persistence_count = 0;
    polarityConfirmed = polarityOfMostRecentVsample;
  }

  if (polarityConfirmed == POSITIVE && polarityConfirmedOfLastSampleV != POSITIVE) {
    if (beyondStartUpPhase) {     
      long realPower_grid_ieu = (sampleSetsDuringThisMainsCycle > 0) ? sumP_grid / sampleSetsDuringThisMainsCycle : 0;
      long realPower_diverted_ieu = (sampleSetsDuringThisMainsCycle > 0) ? sumP_diverted / sampleSetsDuringThisMainsCycle : 0;
      
      energyInBucket_long += (realPower_grid_ieu - requiredExportPerMainsCycle_inIEU);
      if (energyInBucket_long > capacityOfEnergyBucket_long) energyInBucket_long = capacityOfEnergyBucket_long;
      else if (energyInBucket_long < 0) energyInBucket_long = 0;
      
      if (loadStateOfPreviousCycle == LOAD_ON) {
        if (realPower_diverted_ieu >= antiCreepLimit_inIEUperMainsCycle) {
          divertedEnergyRecent_IEU += realPower_diverted_ieu;
          if (divertedEnergyRecent_IEU > IEU_per_Wh) {
            divertedEnergyRecent_IEU -= IEU_per_Wh;
            divertedEnergyTotal_Wh++;
          }
        }
      }

      float grid_export_watts = - (realPower_grid_ieu * settings.powerCal_grid);
      
      if (loadStateOfPreviousCycle == LOAD_ON) {
        isExporting = (grid_export_watts > settings.export_while_diverting_threshold);
      } else {
        isExporting = (grid_export_watts > TANK_FULL_EXPORT_THRESHOLD_WATTS);
      }
      
      if (isExporting) {
        wasExportingInLogPeriod = true;
      }

      if (tankIsFull) {
        if (loadStateOfPreviousCycle == LOAD_ON && realPower_diverted_ieu > antiCreepLimit_inIEUperMainsCycle) {
          tankIsFull = false;
          tankFullDetectionCounter = 0;
        }
      } else {
        if (loadStateOfPreviousCycle == LOAD_ON && realPower_diverted_ieu < antiCreepLimit_inIEUperMainsCycle && isExporting) {
          tankFullDetectionCounter++;
          if (tankFullDetectionCounter > TANK_FULL_DETECTION_CYCLES) {
            tankIsFull = true;
          }
        } else {
          tankFullDetectionCounter = 0;
        }
      }

      if (absenceOfDivertedEnergyCount > accumulatorReset_inMainsCycles) {
        divertedEnergyTotal_Wh = 0; divertedEnergyRecent_IEU = 0; absenceOfDivertedEnergyCount = 0;
      }
      
      sampleCount_forContinuityChecker++;
      if (sampleCount_forContinuityChecker >= CONTINUITY_CHECK_MAXCOUNT) {
        sampleCount_forContinuityChecker = 0;
        logDataPending = true;
      }
    } else {
      startup_MainsCycleCount++;
      if (startup_MainsCycleCount > (CYCLES_PER_SECOND * 5)) {
        beyondStartUpPhase = true;
      }
    }
    sampleSetsDuringThisMainsCycle = 0; sumP_grid = 0; sumP_diverted = 0;
  }
  
  if (sampleSetsDuringThisMainsCycle == 3 && beyondStartUpPhase) {
    if (fullLoadActive) {
      nextStateOfLoad = LOAD_ON;
    } else if (!diverterEnabled) {
      nextStateOfLoad = LOAD_OFF;
    } else {
      if (tankIsFull) {
        if (millis() - tankRetestTimestamp > (unsigned long)TANK_RETEST_INTERVAL_SECONDS * 1000) {
          tankRetestTimestamp = millis();
          nextStateOfLoad = LOAD_ON;
        } else {
          nextStateOfLoad = LOAD_OFF;
        }
      } else {
        if (energyInBucket_long > upperEnergyThreshold_long) {
          nextStateOfLoad = LOAD_ON;
        } else if (energyInBucket_long < lowerEnergyThreshold_long) {
          nextStateOfLoad = LOAD_OFF;
        }
      }
    }
    digitalWrite(outputForTrigger, nextStateOfLoad);
    loadStateOfPreviousCycle = nextStateOfLoad; // Update the state for the next cycle's calculation
    if (nextStateOfLoad == LOAD_ON) { 
      absenceOfDivertedEnergyCount = 0; 
      wasLoadActiveInLogPeriod = true;
    }
    else absenceOfDivertedEnergyCount++;
  }
  
  if (polarityConfirmed == NEGATIVE && polarityConfirmedOfLastSampleV != NEGATIVE) {
    DCoffset_V_long += (cumVdeltasThisCycle_long >> 12);
    cumVdeltasThisCycle_long = 0;
    if (DCoffset_V_long < DCoffset_V_min) DCoffset_V_long = DCoffset_V_min;
    else if (DCoffset_V_long > DCoffset_V_max) DCoffset_V_long = DCoffset_V_max;
  }
  
  long sampleIminusDC_grid = ((long)(sampleI_grid - DCoffset_I)) << 8;
  long filtV_div4 = sampleVminusDC_long >> 2;
  long filtI_div4 = sampleIminusDC_grid >> 2;
  sumP_grid += (filtV_div4 * filtI_div4) >> 12;
  
  if (nextStateOfLoad == LOAD_ON) {
    long sampleIminusDC_diverted = ((long)(sampleI_diverted - DCoffset_I)) << 8;
    filtI_div4 = sampleIminusDC_diverted >> 2;
    sumP_diverted += (filtV_div4 * filtI_div4) >> 12;
  }
  
  sampleSetsDuringThisMainsCycle++;
  cumVdeltasThisCycle_long += sampleVminusDC_long;
  polarityConfirmedOfLastSampleV = polarityConfirmed;
}

void timerIsr(void) {                                         
  static unsigned char sample_index = 0;
  static int sampleV_raw, sampleI_grid_raw, sampleI_diverted_raw;

  switch(sample_index) {
    case 0:
      sampleV_raw = ADC;
      ADMUX = 0x40 + currentSensor_grid;
      sample_index++;
      break;
    case 1:
      sampleI_grid_raw = ADC;
      ADMUX = 0x40 + currentSensor_diverted;
      sample_index++;
      break;
    case 2:
      sampleI_diverted_raw = ADC;
      ADMUX = 0x40 + voltageSensor;
      sample_index = 0;
      processSample(sampleV_raw, sampleI_grid_raw, sampleI_diverted_raw);
      break;
  }
  ADCSRA |= (1 << ADSC);
}

// =================================================================
//   SETUP and LOOP (Entry Points)
// =================================================================
void setup() {  
  pinMode(outputForTrigger, OUTPUT);  
  digitalWrite(outputForTrigger, LOAD_OFF);
  pinMode(outputModeSelectorPin, INPUT_PULLUP);
 
  Serial.begin(9600);
  while(!Serial) {;}
  Serial.println("\n-------------------------------------");
  Serial.println("PV Diverter Initializing (ISR-Driven)...");
  
  wdt_enable(WDTO_2S);
  Serial.println("Watchdog timer enabled (2s timeout).");

  loadSettings();

  DCoffset_V_long = 512L * 256;
  DCoffset_V_min = (long)(512L - 100) * 256;
  DCoffset_V_max = (long)(512L + 100) * 256;

  ADCSRA = (1 << ADPS0) | (1 << ADPS1) | (1 << ADPS2);
  ADCSRA |= (1 << ADEN);

  Timer1.initialize(ADC_TIMER_PERIOD);
  Timer1.attachInterrupt(timerIsr);

  Serial.println("Initialization Complete. Type 'HELP' for commands.");
  Serial.println("-------------------------------------");
}

void loop() { 
  wdt_reset();
  handleSerialCommands();
  checkOutputModeSwitch();

  if (logDataPending) {
    logData();
    logDataPending = false;
  }
}
