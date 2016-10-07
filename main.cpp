/*
 * HM-Sensor.cpp
 *
 * Created: 27.12.2015 14:47:56
 * Author : Martin
 */ 

//- load library's --------------------------------------------------------------------------------------------------------
#include <Arduino.h>
#include "AS.h"																				// ask sin framework
#include "register.h"																		// configuration sheet
#include "dht.h"
#include "OneWire.h"
#include <avr/wdt.h>

//#define SER_DBG

#define DHT_PWR		9																		// power for DHT22
#define DHT_PIN		6																		// this pin DHT22 is connected to
#define OW_PIN		5																		// this pin DS18B20 is connected to

#include "00_debug-flag.h"


dht DHT;
OneWire OW(OW_PIN);
waitTimer thTimer;
int16_t celsius;
uint8_t transmitDevTryMax;

void serialEvent();


//- arduino functions -----------------------------------------------------------------------------------------------------
void setup() {

	// - Hardware setup ---------------------------------------
	// - everything off ---------------------------------------
	
	wdt_disable();																			// clear WDRF to avoid endless resets after WDT reset
	MCUSR &= ~(1<<WDRF);																	// stop all WDT activities
	WDTCSR |= (1<<WDCE) | (1<<WDE);
	WDTCSR = 0x00;

	EIMSK = 0;																				// disable external interrupts
	ADCSRA = 0;																				// ADC off
	power_all_disable();																	// and everything else
	
	// todo: timer0 and SPI should enable internally
	power_timer0_enable();
	power_spi_enable();																		// enable only needed functions


	// enable only what is really needed

	#ifdef SER_DBG																			// some debug
		DBG_START(SER, F("SER.\n") );														// ...some debug

		//dbgStart();																		// serial setup
		DBG(SER, F("\nHB_UW_Sen_TH_Pn\n"));
		DBG(SER, F(LIB_VERSION_STRING));													// ...and some information
	#endif
	
	// - AskSin related ---------------------------------------
	hm.init();																				// init the asksin framework
	sei();																					// enable interrupts

	// - user related -----------------------------------------
	#ifdef SER_DBG
		DBG(SER, F("HMID: "), _HEX(dev_ident.HMID,3), F(", MAID: "), _HEX(MAID,3), F("\n\n"));	// some debug
	#endif
}


//- user functions --------------------------------------------------------------------------------------------------------
void initTH(uint8_t channel) {																// init the sensor
	
	pinMode(DHT_PWR, OUTPUT);
	
	#ifdef SER_DBG
		DBG(SER, F("init th: cnl: "), channel, F("\n"));
	#endif
}

// this is called when HM wants to send measured values to peers or master
// due to asynchronous measurement we simply can take the values very quick from variables
void measureTH(uint8_t channel, cmTHSensWeather::s_sensVal *sensVal) {
	int16_t t;
	
	#ifdef SER_DBG
		//dbg << "msTH DS-t: " << celsius << ' ' << _TIME << '\n';
	#endif
	// take temp value from DS18B20
	t = celsius / 10;
	((uint8_t *)&(sensVal->temp))[0] = ((t >> 8) & 0x7F);									// battery status is added later
	((uint8_t *)&(sensVal->temp))[1] = t & 0xFF;
	
	#ifdef SER_DBG
		//dbg << "msTH t: " << DHT.temperature << ", h: " << DHT.humidity << ' ' << _TIME << '\n';
	#endif
	// take humidity value from DHT22
	sensVal->hum = DHT.humidity / 10;
	// fetch battery voltage
	t = bat.getVolts();
	((uint8_t *)&(sensVal->bat))[0] = t >> 8;
	((uint8_t *)&(sensVal->bat))[1] = t & 0xFF;
}

void cnl0Change(void) {
	
	// set lowBat threshold
	bat.set(*ptr_CM[0]->list[0]->ptr_to_val(REG_CHN0_LOW_BAT_LIMIT_TH)*10, BATTERY_MEAS_INTERVAL);
	//bat.set(ee_list.getRegAddr(0,0,0,REG_CHN0_LOW_BAT_LIMIT_TH)*10, BATTERY_MEAS_INTERVAL);

	// set OSCCAL frequency
	if (uint8_t oscCal = *ptr_CM[0]->list[0]->ptr_to_val(REG_CHN0_OSCCAL)) {
	//if (uint8_t oscCal = ee_list.getRegAddr(0,0,0,REG_CHN0_OSCCAL)) {
	#ifdef SER_DBG
		dbg << F("will set OSCCAL: old=") << OSCCAL << F(", new=") << oscCal << F("\n");
	#endif
		// Attention: your controller my have other factory calibration !!
		// If you are unsure about the internal RC-frequency of your chip then use factory default (oscCal-Reg = 0)
		// my chip: 1kHz - 8A=994Hz, 8B=998,4Hz, 8C=1001,6Hz, 8E=1010Hz
		// frequency measured with help of millis-ISR (toggling LED port and measuring frequency on it)

		//OSCCAL = oscCal;
	} else {
	#ifdef SER_DBG
		dbg << F("will set default OSCCAL: ") << getDefaultOSCCAL() << F("\n");
	#endif
		OSCCAL = getDefaultOSCCAL();
	}
	calibrateWatchdog();

	// if burstRx is set ...
	if (*ptr_CM[0]->list[0]->ptr_to_val(REG_CHN0_BURST_RX)) {
	//if (ee_list.getRegAddr(0,0,0,REG_CHN0_BURST_RX)) {
	#ifdef SER_DBG
		dbg << F("PM=onradio\n");
	#endif
		pom.setMode(POWER_MODE_WAKEUP_ONRADIO);											// set mode to wakeup on burst
	} else {	// no burstRx wanted
		#ifdef SER_DBG
			//dbg << F("PM=8000ms\n");
			dbg << F("PM: no sleep\n");
		#endif
			//pom.setMode(POWER_MODE_WAKEUP_8000MS);										// set mode to awake every 8 secs
			pom.setMode(POWER_MODE_NO_SLEEP);
	}

	// fetch transmitDevTryMax
	if ((transmitDevTryMax = *ptr_CM[0]->list[0]->ptr_to_val(REG_CHN0_TRANS_DEV_TRY_MAX)) > 10)
	//if ((transmitDevTryMax = ee_list.getRegAddr(0,0,0,REG_CHN0_TRANS_DEV_TRY_MAX)) > 10)
		transmitDevTryMax = 10;
	else if (transmitDevTryMax < 1)
		transmitDevTryMax = 1;
}

// this is called regularly - real measurement is done here
void measure() {
	enum mState {mInit, mWait, mPwrOn, mStartDS};
	static mState state = mWait;

	if (!thTimer.done())
		return;
		
	if (state == mInit) {																	// wait some time till next measurement
		thTimer.set(88000);																	// measurement every 90 secs
		state = mWait;
	}
	else if (state == mWait) {																// power on sensor and wait 1 sec
		thTimer.set(1000);
		pinMode(OW_PIN, INPUT_PULLUP);
		digitalWrite(DHT_PWR, 1);															// power on here

		state = mPwrOn;
		#ifdef SER_DBG
			//dbg << "power on Sensor" << ' ' << _TIME << '\n';
		#endif
	}
	else if (state == mPwrOn) {																// now start measurement on DS18B20 and wait another second
		thTimer.set(1000);
		uint8_t rc = OW.reset();															// attention - OW device get ready to communicate!
		OW.skip();																			// skip rom selection - we have only one device attached
		OW.write(0x44);																		// start conversion
		#ifdef SER_DBG
			//dbg << "rc: " << rc << _TIME << '\n';
		#endif
		state = mStartDS;
	}
	else if (state == mStartDS)	{															// get results here and switch off sensor
		DHT.read22(DHT_PIN);																// read DHT22
		#ifdef SER_DBG
			dbg << "t: " << DHT.temperature << ", h: " << DHT.humidity << ' ' << _TIME << '\n';
		#endif
		
		OW.reset();																			// attention - get ready to read result from DS18B20
		OW.skip();																			// no rom selection
		OW.write(0xBE);																		// read temp from scratchpad
		celsius = ((uint32_t)(OW.read() | (OW.read() << 8)) * 100) >> 4;					// we need only first two bytes from scratchpad
	
		#ifdef SER_DBG
			dbg << "DS-t: " << celsius << ' ' << _TIME << '\n';
		#endif

		digitalWrite(DHT_PWR, 0);															// power off sensor
		pinMode(OW_PIN, INPUT);
		pinMode(DHT_PIN, INPUT);
		
		#ifdef SER_DBG
			//dbg << "power off Sensor" << ' ' << _TIME << '\n';
		#endif
		state = mInit;
	}
}


int main(void)
{
	// Initialize all functions and pins
	setup();
	
    /* Replace with your application code */
    while (1) 
    {
			// - AskSin related ---------------------------------------
			hm.poll();																		// poll the homematic main loop

			// - user related -----------------------------------------
			serialEvent();
			measure();
    }
}


//- predefined functions --------------------------------------------------------------------------------------------------
void serialEvent() {
	#ifdef SER_DBG
	
	static uint8_t i = 0;																	// it is a high byte next time
	while (Serial.available()) {
		uint8_t inChar = (uint8_t)Serial.read();											// read a byte
		if (inChar == '\n') {																// send to receive routine
			i = 0;
			snd_msg.active = 1;
		}
		
		if      ((inChar>96) && (inChar<103)) inChar-=87;									// a - f
		else if ((inChar>64) && (inChar<71))  inChar-=55;									// A - F
		else if ((inChar>47) && (inChar<58))  inChar-=48;									// 0 - 9
		else continue;
		
		if (i % 2 == 0) snd_msg.buf[i/2] = inChar << 4;										// high byte
		else snd_msg.buf[i/2] |= inChar;														// low byte
		
		i++;
	}
	#endif
}
