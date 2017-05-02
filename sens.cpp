#include "FreeRTOS.h"
#include "task.h"

#include "parameters.h"

#include "sens.h"

#include "uart1.h"
#include "main.h"
#include "ctrl.h"
#include "gps.h"
#include "knob.h"

#include "i2c.h"

#if defined(WITH_BMP180) || defined(WITH_BMP280)

#ifdef WITH_BMP180
#include "bmp180.h"
#endif

#ifdef WITH_BMP280
#include "bmp280.h"
#endif

#include "atmosphere.h"
#include "slope.h"
#include "lowpass2.h"
#include "intmath.h"

#include "fifo.h"

// static const uint8_t  VarioVolume     =    2; // [0..3]
static const uint16_t VarioBasePeriod = 800;  // [ms]

#ifdef WITH_BEEPER
void VarioSound(int32_t ClimbRate)
{
  uint8_t VarioVolume = KNOB_Tick>>1; if(VarioVolume>3) VarioVolume=3;  // take vario volume from the user knob
  if(ClimbRate>=50)                                                     // if climb > 1/2 m/s
  { uint8_t Note=(ClimbRate-50)/50;                                     // one semitone per 1/2 m/s
    if(Note>=0x0F) Note=0x0F;                                           // stop at 15 thus 8 m/s
    uint16_t Period=(VarioBasePeriod+(Note>>1))/(1+Note);               // beeping period
    Vario_Period=Period; Vario_Fill=Period>>1;                          // period shorter (faster beeping) with stronger climb
    Vario_Note = (VarioVolume<<6) | (0x10+Note); }                      // note to play: higher for stronger climb
  else if(ClimbRate<=(-100))                                            // if sink > 1 m/s
  { uint8_t Note=(-ClimbRate-100)/100;                                  // one semitone per 1 m/s
    if(Note>=0x0B) Note=0x0B;                                           //
    Vario_Period=VarioBasePeriod; Vario_Fill=VarioBasePeriod;           // continues tone
    Vario_Note = (VarioVolume<<6) | (0x0B-Note); }                      // note to play: lower for stronger sink
  else                                                                  // if climb less than 1/2 m/s or sink less than 1 m/s
  { Vario_Note=0x00; }                                                  // no sound
}
#endif // WITH_BEEPER

#ifdef WITH_BMP180
static BMP180   Baro;                       // BMP180 barometer sensor
#endif

#ifdef WITH_BMP280
static BMP280   Baro;                       // BMP280 barometer sensor
#endif

static uint32_t AverPress;                  // [ Pa] summed Pressure over several readouts
static uint8_t  AverCount;                  // [int] number of summed Pressure readouts

static SlopePipe<int32_t>        BaroPipe;  // 4-point slope-fit pipe for pressure
static LowPass2<int32_t,6,4,8>   BaroNoise; // low pass (average) filter for pressure noise

static LowPass2<int64_t,10,9,12> PressAver, // low pass (average) filter for pressure
                                 AltAver;   // low pass (average) filter for GPS altitude

static Delay<int32_t, 8>        PressDelay; // 4-second delay for long-term climb rate

static char Line[64];                       // line to prepare the barometer NMEA sentence

static bool InitBaro()
{ Baro.Bus=BARO_I2C;
  uint8_t Err=Baro.CheckID();
  if(Err==0) Err=Baro.ReadCalib();
#ifdef WITH_BMP180
  if(Err==0) Err=Baro.AcquireRawTemperature();
  if(Err==0) { Baro.CalcTemperature(); AverPress=0; AverCount=0; }
#endif
#ifdef WITH_BMP280
  if(Err==0) Err=Baro.Acquire();
  if(Err==0) { Baro.Calculate(); }
#endif
  return Err==0; }

static void ProcBaro()
{
    static uint8_t PipeCount=0;

    int16_t Sec  = 10*GPS_Sec;                                            // [0.1sec]
    uint16_t Phase = PPS_Phase();                                         // sync to the GPS PPS
    if(Phase>=500) { Sec+=10; vTaskDelay(1000-Phase); }
              else { Sec+= 5; vTaskDelay( 500-Phase); }
    if(Sec>=600) Sec-=600;

#ifdef WITH_BMP180
    TickType_t Start=xTaskGetTickCount();
    uint8_t Err=Baro.AcquireRawTemperature();                             // measure temperature
    if(Err==0) { Baro.CalcTemperature(); AverPress=0; AverCount=0; }      // clear the average
          else { PipeCount=0;
	         I2C_Restart(Baro.Bus, 400000); vTaskDelay(20); InitBaro(); // try to recover I2C bus and baro
		 return; }

    for(uint8_t Idx=0; Idx<24; Idx++)
    { uint8_t Err=Baro.AcquireRawPressure();                              // take pressure measurement
      if(Err==0) { Baro.CalcPressure(); AverPress+=Baro.Pressure; AverCount++; } // sum-up average pressure
      TickType_t Time=xTaskGetTickCount()-Start; if(Time>=450) break; }   // but no longer than 450ms to fit into 0.5 second slot

    if(AverCount==0) { PipeCount=0; return ; }                          // and we summed-up some measurements
    AverPress = ( (AverPress<<2) + (AverCount>>1) )/AverCount;          // [0.25Pa]] make the average
#endif
#ifdef WITH_BMP280
    uint8_t Err=Baro.Acquire();
    if(Err==0) { Baro.Calculate(); }
          else { PipeCount=0; return; }
            Err=Baro.Acquire();
    AverPress = Baro.Pressure;                                          // [0.25Pa]
    if(Err==0) { Baro.Calculate(); }
          else { PipeCount=0; return; }
    AverPress = (Baro.Pressure+AverPress)/2;                            // [0.25Pa]
#endif

    BaroPipe.Input(AverPress);                                          // [0.25Pa]
    if(PipeCount<255) PipeCount++;                                      // count data going to the slope fitting pipe
    if(PipeCount<4) return;

    BaroPipe.FitSlope();                                             // fit the average and slope from the four most recent pressure points
    int32_t PLR = Atmosphere::PressureLapseRate(((AverPress+2)>>2), Baro.Temperature); // [0.0001m/Pa]
    int32_t ClimbRate = (BaroPipe.Slope*PLR)/800;                    // [1/16Pa/0.5sec] * [0.0001m/Pa] x 800 => [0.01m/sec]

    BaroPipe.CalcNoise();                                            // calculate the noise (average square residue)
    uint32_t Noise=BaroNoise.Process(BaroPipe.Noise);                // pass the noise through the low pass filter
             Noise=(IntSqrt(25*Noise)+64)>>7;                        // [0.1 Pa] noise (RMS) measured on the pressure

    int32_t Pressure=BaroPipe.Aver;                                  // [1/16Pa]
    int32_t StdAltitude = Atmosphere::StdAltitude((Pressure+8)>>4);  // [0.1 m]
    int32_t ClimbRate4sec = ((Pressure-PressDelay.Input(Pressure))*PLR)/3200; // [0.01m/sec] climb rate over 4 sec.
#ifdef WITH_VARIO
    VarioSound(ClimbRate);
    // if(abs(ClimbRate4sec)>50) VarioSound(ClimbRate);
    //                      else VarioSound(2*ClimbRate4sec);
#endif
    Pressure = (Pressure+2)>>2;                                      // [0.25 Pa]
    if( (Phase>=500) && GPS_TimeSinceLock )
    { PressAver.Process(Pressure);                                   // [0.25 Pa] pass pressure through low pass filter
      AltAver.Process(GPS_Altitude);                                 // [0.1 m] pass GPS altitude through same low pass filter
    }
    int32_t PressDiff=Pressure-((PressAver.Out+2048)>>12);           // [0.25 Pa] pressure - <pressure>
    int32_t AltDiff = (PressDiff*(PLR>>4))/250;                      // [0.1 m]
    int32_t Altitude=((AltAver.Out+2048)>>12)+AltDiff;               // [0.1 m]

    uint8_t Frac = Sec%10;
    if(Frac==0)
    { GPS_Position *PosPtr = GPS_getPosition(Sec/10);                 // get GPS position record for this second
      if(PosPtr)                                                     // if found:
      { PosPtr->StdAltitude = StdAltitude;                           // store standard pressure altitude
        PosPtr->Temperature = Baro.Temperature;                      // and temperature in the GPS record
        PosPtr->setBaro(); }
    }

    uint8_t Len=0;                                                   // start preparing the barometer NMEA sentence
    Len+=Format_String(Line+Len, "$POGNB,");
    Len+=Format_UnsDec(Line+Len, Sec, 3, 1);                         // [sec] measurement time
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, Baro.Temperature, 2, 1);           // [degC] temperature
    Line[Len++]=',';
    Len+=Format_UnsDec(Line+Len, (uint32_t)(10*Pressure+2)>>2, 2, 1); // [Pa] pressure
    Line[Len++]=',';
    Len+=Format_UnsDec(Line+Len, Noise, 2, 1);                       // [Pa] pressure noise
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, StdAltitude, 2, 1);                // [m] standard altitude (calc. from pressure)
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, Altitude,    2, 1);                // [m] altitude (from cross-calc. with the GPS)
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, ClimbRate,   3, 2);                // [m/s] climb rate
    Line[Len++]=',';
    Len+=NMEA_AppendCheckCRNL(Line, Len);
    xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
    Format_String(CONS_UART_Write, Line, Len);                           // send NMEA sentence to the console (UART1)
    xSemaphoreGive(CONS_Mutex);

    Len=0;                                                           // start preparing the PGRMZ NMEA sentence
    Len+=Format_String(Line+Len, "$PGRMZ,");
    Len+=Format_SignDec(Line+Len, StdAltitude, 2, 1);                // [m] standard altitude (calc. from pressure)
    Line[Len++]=',';
    Len+=Format_String(Line+Len, "m,");                              // normally f for feet, but metres and m works with XcSoar
    Len+=Format_String(Line+Len, "3");                               // 1 no fix, 2 - 2D, 3 - 3D; assume 3D for now
    Len+=NMEA_AppendCheckCRNL(Line, Len);
    xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
    Format_String(CONS_UART_Write, Line, Len);                           // send NMEA sentence to the console (UART1)
    xSemaphoreGive(CONS_Mutex);

#ifdef WITH_SDLOG
    xSemaphoreTake(Log_Mutex, portMAX_DELAY);
    Format_String(Log_Write, Line, Len);                             // send NMEA sentence to the log file
    xSemaphoreGive(Log_Mutex);
#endif

}

#endif // WITH_BMP180/BMP280



extern "C"
void vTaskSENS(void* pvParameters)
{ vTaskDelay(20);   // this delay seems to be essential - if you don't wait long enough, the BMP180 won't respond properly.

// #ifdef WITH_BEEPER
//   VarioSound(0);
// #endif

  BaroPipe.Clear  (4*90000);
  BaroNoise.Set(12*16);                // guess the pressure noise level

                                       // initialize the GPS<->Baro correlator
  AltAver.Set(0);                      // [m] Altitude at sea level
  PressAver.Set(4*101300);             // [Pa] Pressure at sea level
  PressDelay.Clear(4*101300);

  bool Detected = InitBaro();

  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "TaskSENS:");

#ifdef WITH_BMP180
  Format_String(CONS_UART_Write, " BMP180: ");
  if(Detected) Format_String(CONS_UART_Write, " detected");
         else  Format_String(CONS_UART_Write, " not there !");
#endif

#ifdef WITH_BMP280
  Format_String(CONS_UART_Write, " BM2180: ");
  if(Detected) Format_String(CONS_UART_Write, " detected");
         else  Format_String(CONS_UART_Write, " not there !");
#endif

  Format_String(CONS_UART_Write, "\n");
  xSemaphoreGive(CONS_Mutex);

  while(1)
  {
#if defined(WITH_BMP180) || defined(WITH_BMP280)
    ProcBaro();
#else
    vTaskDelay(1000);
#endif
  }
}

