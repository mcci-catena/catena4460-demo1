/* catena4460-demo1.ino	Tue Mar 28 2017 19:52:20 tmm */

/*

Module:  catena4460-demo1.ino

Function:
	Demonstrate the Catena 4460's built-in sensors.

Copyright notice:
	This file copyright (C) 2017-2018 by

		MCCI Corporation
		3520 Krums Corners Road
		Ithaca, NY  14850

	An unpublished work.  All rights reserved. The license file
        accompanying this file outlines the license granted.

Author:
	Terry Moore, MCCI Corporation & Suzen Filke	December 2018

Version and revision:
        See https://github.com/mcci-catena/catena4460-demo1

*/

#include <Catena.h>
#include <CatenaRTC.h>
#include <Catena_Led.h>
#include <Catena_TxBuffer.h>
#include <Catena_CommandStream.h>
#include <Catena_Totalizer.h>

#include <Wire.h>
#include <Adafruit_BME680.h>
#include <Arduino_LoRaWAN.h>
#include <BH1750.h>
#include <lmic.h>
#include <hal/hal.h>
#include <mcciadk_baselib.h>
#include <delay.h>

#include <cmath>
#include <type_traits>

/****************************************************************************\
|
|		Manifest constants & typedefs.
|
|	This is strictly for private types and constants which will not
|	be exported.
|
\****************************************************************************/

using namespace McciCatena;

/* how long do we wait between measurements (in seconds) */
enum    {
        // set this to interval between measurements, in seconds
        // Actual time will be a little longer because have to
        // add measurement and broadcast time.
        CATCFG_T_CYCLE = 30,        // every 30 seconds
        };

/* additional timing parameters */
enum    {
        CATCFG_T_WARMUP = 1,
        CATCFG_T_SETTLE = 5,
        CATCFG_T_INTERVAL = CATCFG_T_CYCLE - (CATCFG_T_WARMUP +
                                                CATCFG_T_SETTLE),
        };

// forwards
static void settleDoneCb(osjob_t *pSendJob);
static void warmupDoneCb(osjob_t *pSendJob);
static void txFailedDoneCb(osjob_t *pSendJob);
static void sleepDoneCb(osjob_t *pSendJob);
static Arduino_LoRaWAN::SendBufferCbFn sendBufferDoneCb;

/****************************************************************************\
|
|	Read-only data.
|
|	If program is to be ROM-able, these must all be tagged read-only
|	using the ROM storage class; they may be global.
|
\****************************************************************************/

static const char sVersion[] = "0.1.1";

/****************************************************************************\
|
|	VARIABLES:
|
|	If program is to be ROM-able, these must be initialized
|	using the BSS keyword.  (This allows for compilers that require
|	every variable to have an initializer.)  Note that only those
|	variables owned by this module should be declared here, using the BSS
|	keyword; this allows for linkers that dislike multiple declarations
|	of objects.
|
\****************************************************************************/

// globals
Catena gCatena;

//
// the LoRaWAN backhaul.  Note that we use the
// Catena version so it can provide hardware-specific
// information to the base class.
//
Catena::LoRaWAN gLoRaWAN;

//
// the LED
//
StatusLed gLed (Catena::PIN_STATUS_LED);

// the RTC instance, used for sleeping
CatenaRTC gRtc;

//   The temperature/humidity sensor
Adafruit_BME680 bme; // The default initalizer creates an I2C connection
bool fBme;

//   The LUX sensor
BH1750 bh1750;
bool fLux;

//  the LMIC job that's used to synchronize us with the LMIC code
static osjob_t sensorJob;
void sensorJob_cb(osjob_t *pJob);

/*

Name:	setup()

Function:
        Arduino setup function.

Definition:
        void setup(
            void
            );

Description:
	This function is called by the Arduino framework after
	basic framework has been initialized. We initialize the sensors
	that are present on the platform, set up the LoRaWAN connection,
        and (ultimately) return to the framework, which then calls loop()
        forever.

Returns:
	No explicit result.

*/

void setup(void)
        {
        gCatena.begin();

        gCatena.SafePrintf("Catena 4450 sensor1 V%s\n", sVersion);

        gLed.begin();
        gCatena.registerObject(&gLed);

        // set up the RTC object
        gRtc.begin();

        gCatena.SafePrintf("LoRaWAN init: ");
        if (!gLoRaWAN.begin(&gCatena))
                {
                gCatena.SafePrintf("failed\n");
                gCatena.registerObject(&gLoRaWAN);
                }
        else
                {
                gCatena.SafePrintf("OK\n");
                gCatena.registerObject(&gLoRaWAN);
                }

        Catena::UniqueID_string_t CpuIDstring;

        gCatena.SafePrintf("CPU Unique ID: %s\n",
                gCatena.GetUniqueIDstring(&CpuIDstring)
                );

        /* find the platform */
        const Catena::EUI64_buffer_t *pSysEUI = gCatena.GetSysEUI();

        uint32_t flags;
        const CATENA_PLATFORM * const pPlatform = gCatena.GetPlatform();

        if (pPlatform)
                {
                gCatena.SafePrintf("EUI64: ");
                for (unsigned i = 0; i < sizeof(pSysEUI->b); ++i)
                        {
                        gCatena.SafePrintf("%s%02x", i == 0 ? "" : "-", pSysEUI->b[i]);
                        }
                gCatena.SafePrintf("\n");
                flags = gCatena.GetPlatformFlags();
                gCatena.SafePrintf(
                        "Platform Flags:  %#010x\n",
                        flags
                        );
                gCatena.SafePrintf(
                        "Operating Flags:  %#010x\n",
                        gCatena.GetOperatingFlags()
                        );
                }
        else
                {
                gCatena.SafePrintf("**** no platform, check provisioning ****\n");
                flags = 0;
                }


        /* initialize the lux sensor */
        if (flags & CatenaSamd21::fHasLuxRohm)
                {
                bh1750.begin();
                fLux = true;
                bh1750.configure(BH1750_CONTINUOUS_HIGH_RES_MODE_2);
                }
        else
                {
                fLux = false;
                }

        /* initialize the BME680 */
        if (! /* bme.begin(BME680_ADDRESS, Adafruit_BME680::OPERATING_MODE::Sleep) */
            bme.begin()) // the Adafruit BME680 lib doesn't have the MCCI low-power hacks
                {
                gCatena.SafePrintf("No BME680 found: check wiring\n");
                fBme = false;
                }
        else
                {
                fBme = true;
                }

        /* is it modded? */
        uint32_t modnumber = gCatena.PlatformFlags_GetModNumber(flags);

        if (modnumber != 0)
                {
                gCatena.SafePrintf("Catena 4460-M%u\n", modnumber);
                }
        else
                {
                gCatena.SafePrintf("No mods detected\n");
                }

        /* now, we kick off things by sending our first message */
        gLed.Set(LedPattern::Joining);

        /* warm up the BME680 by discarding a measurement */
        if (fBme)
                (void)bme.readTemperature();

        /* trigger a join by sending the first packet */
        startSendingUplink();
        }

// The Arduino loop routine -- in our case, we just drive the other loops.
// If we try to do too much, we can break the LMIC radio. So the work is
// done by outcalls scheduled from the LMIC os loop.
void loop()
        {
        gCatena.poll();
        }

void startSendingUplink(void)
{
  TxBuffer_t b;
  LedPattern savedLed = gLed.Set(LedPattern::Measuring);

  b.begin();
  FlagsSensor2 flag;

  flag = FlagsSensor2(0);

  b.put(FormatSensor2); /* the flag for this record format */
  
  /* capture the address of the flag byte */
  uint8_t * const pFlag = b.getp();
  
  b.put(0x00);  /* insert a byte. Value doesn't matter, will 
                || later be set to the final value of `flag`
                */

  // vBat is sent as 5000 * v
  float vBat = gCatena.ReadVbat();
  gCatena.SafePrintf("vBat:    %d mV\n", (int) (vBat * 1000.0f));
  b.putV(vBat);
  flag |= FlagsSensor2::FlagVbat;

  uint32_t bootCount;
  if (gCatena.getBootCount(bootCount))
        {
        b.putBootCountLsb(bootCount);
        flag |= FlagsSensor2::FlagBoot;
        }

  if (fBme)
       {
       // bme.performReading() loads a consistent measurement into 
       // the bme. 
       //
       // temperature is 2 bytes from -0x80.00 to +0x7F.FF degrees C
       // pressure is 2 bytes, hPa * 10.
       // humidity is one byte, where 0 == 0/256 and 0xFF == 255/256.
       bme.performReading();
       gCatena.SafePrintf(
                "BME680:  T: %d P: %d RH: %d\n",
                (int) bme.temperature,
                (int) bme.pressure,
                (int) bme.humidity
                );
       b.putT(bme.temperature);
       b.putP(bme.pressure);
       b.putRH(bme.humidity);

       flag |= FlagsSensor2::FlagTPH;
       }

  if (fLux)
        {
        /* Get a new sensor event */
        uint16_t light;

        light = bh1750.readLightLevel();
        gCatena.SafePrintf("BH1750:  %u lux\n", light);
        b.putLux(light);
        flag |= FlagsSensor2::FlagLux;
        }

  *pFlag = uint8_t(flag);
  if (savedLed != LedPattern::Joining)
          gLed.Set(LedPattern::Sending);
  else
          gLed.Set(LedPattern::Joining);

  gLoRaWAN.SendBuffer(b.getbase(), b.getn(), sendBufferDoneCb, NULL);
}

static void
sendBufferDoneCb(
    void *pContext,
    bool fStatus
    )
    {
    osjobcb_t pFn;

    gLed.Set(LedPattern::Settling);
    if (! fStatus)
        {
        gCatena.SafePrintf("send buffer failed\n");
        pFn = txFailedDoneCb;
        }
    else
        {
        pFn = settleDoneCb;
        }
    os_setTimedCallback(
            &sensorJob,
            os_getTime()+sec2osticks(CATCFG_T_SETTLE),
            pFn
            );
    }

static void
txFailedDoneCb(
        osjob_t *pSendJob
        )
        {
        gCatena.SafePrintf("not provisioned, idling\n");
        gLoRaWAN.Shutdown();
        gLed.Set(LedPattern::NotProvisioned);
        }


//
// the following API is added to delay.c, .h in the BSP. It adjust millis()
// forward after a deep sleep.
//
// extern "C" { void adjust_millis_forward(unsigned); };
//
// If you don't have it, check the following commit at github:
// https://github.com/mcci-catena/ArduinoCore-samd/commit/78d8440dbcd29bf5ac659fd65514268c1334f683
//

static void settleDoneCb(
    osjob_t *pSendJob
    )
    {
    uint32_t startTime;

    // if connected to USB, don't sleep
    // ditto if we're monitoring pulses.

    // enable/disable sleep when connected to USB
    //if (Serial.dtr())
    if (Serial.dtr() || true)
        {
        gLed.Set(LedPattern::Sleeping);
        os_setTimedCallback(
                &sensorJob,
                os_getTime() + sec2osticks(CATCFG_T_INTERVAL),
                sleepDoneCb
                );
        return;
        }

    /* ok... now it's time for a deep sleep */
    gLed.Set(LedPattern::Off);

    startTime = millis();
    gRtc.SetAlarm(CATCFG_T_INTERVAL);
    gRtc.SleepForAlarm(
        gRtc.MATCH_HHMMSS,
        gRtc.SleepMode::IdleCpuAhbApb
        );

    // add the number of ms that we were asleep to the millisecond timer.
    // we don't need extreme accuracy.
    adjust_millis_forward(CATCFG_T_INTERVAL  * 1000);

    /* and now... we're awake again. trigger another measurement */
    sleepDoneCb(pSendJob);
    }

static void sleepDoneCb(
        osjob_t *pJob
        )
        {
        gLed.Set(LedPattern::WarmingUp);

        os_setTimedCallback(
                &sensorJob,
                os_getTime() + sec2osticks(CATCFG_T_WARMUP),
                warmupDoneCb
                );
        }

static void warmupDoneCb(
    osjob_t *pJob
    )
    {
    startSendingUplink();
    }