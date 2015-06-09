#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include <string.h>

#include "ctrl.h"

#include "uart1.h"       // UART1 (console)
#include "nmea.h"        // NMEA
#include "parameters.h"  // Parameters in Flash
#include "format.h"      // output formatting

#include "main.h"
#include "gps.h"

SemaphoreHandle_t UART1_Mutex;            // Console port Mutex

#include "diskio.h"
#include "ff.h"

uint32_t get_fattime(void) { return GPS_FatTime; } // for FatFS to have the correct time

// ======================================================================================

static char Line[64];

static void PrintParameters(void)                               // print parameters stored in Flash
{ Parameters.Print(Line);
  xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
  Format_String(UART1_Write, Line);
  xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks
}

static void ProcessCtrlC(void)                                  // print system state to the console
{ 
  PrintParameters();

  size_t FreeHeap = xPortGetFreeHeapSize();

  xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
  Format_String(UART1_Write, "Task  Pr. Stack, ");
  Format_UnsDec(UART1_Write, (uint32_t)FreeHeap, 4, 3);
  Format_String(UART1_Write, "kB free\n");
  xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks

  UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
  TaskStatus_t *pxTaskStatusArray = (TaskStatus_t *)pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) );
  if(pxTaskStatusArray==0) return;
  uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, NULL );
  for(UBaseType_t T=0; T<uxArraySize; T++)
  { TaskStatus_t *Task = pxTaskStatusArray+T;
    // uint8_t Len=strlen(Task->pcTaskName);
    // memcpy(Line, Task->pcTaskName, Len);
    uint8_t Len=Format_String(Line, Task->pcTaskName);
    for( ; Len<=configMAX_TASK_NAME_LEN; )
      Line[Len++]=' ';
    Line[Len++]='0'+Task->uxCurrentPriority; Line[Len++]=' ';
    Len+=Format_UnsDec(Line+Len, Task->usStackHighWaterMark, 3);
    Line[Len++]='\n'; Line[Len++]=0;
    xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
    Format_String(UART1_Write, Line);
    xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks
  }
  vPortFree( pxTaskStatusArray );
}

static NMEA_RxMsg NMEA;

static void ReadParameters(void)  // read parameters requested by the user in the NMEA sent.
{
}

static void ProcessNMEA(void)     // process a valid NMEA that got to the console
{ }

static void ProcessInput(void)
{
  for( ; ; )
  { uint8_t Byte; int Err=UART1_Read(Byte); if(Err<=0) break; // get byte from console, if none: exit the loop
    if(Byte==0x03) ProcessCtrlC();                            // if Ctrl-C received
    NMEA.ProcessByte(Byte);                                   // pass the byte through the NMEA processor
    if(NMEA.isComplete())                                     // if complete NMEA:
    { if(NMEA.isChecked()) ProcessNMEA();                     // and if CRC is good: interpret the NMEA
      NMEA.Clear(); }                                         // clear the NMEA processor for the next sentence
  }
}

#ifdef LOG_ENABLE
static   uint16_t  LogDate     =              0;                  // [days] date = UnixTime/86400
static       char  LogName[14] = "TRK00000.LOG";                  // log file name
static FRESULT     LogErr;                                        // most recent error/state of the logging system
static FATFS       FatFs;                                         // FatFS object for the file system (FAT)
static FIL         LogFile;                                       // FatFS object for the log file
static TickType_t  LogOpenTime;                                   // when was the log file (re)open
xQueueHandle       LogQueue;                                      // queue that stores pointers to lines to be written to the log file

void Log_Open(void)
{ LogDate=get_fattime()/86400;                                    // get the unix date
  Format_UnsDec(LogName+3, LogDate, 5);                           // format the date into the log file name
  LogErr=f_open(&LogFile, LogName, FA_WRITE | FA_OPEN_ALWAYS);    // open the log file
  if(LogErr)
  { // xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
    // Format_String(UART1_Write, "TaskCTRL: error when openning "); // report open error
    // Format_String(UART1_Write, LogName);
    // Format_String(UART1_Write, "\n");
    // xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks
    return ; }
  LogErr=f_lseek(&LogFile, f_size(&LogFile));                     // move to the end of the file (for append)
  LogOpenTime=xTaskGetTickCount();                                // record the system time when log was open

  if(!LogErr)
  { 
    xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
    Format_String(UART1_Write, "TaskCTRL: writing to ");          // report open file name
    Format_String(UART1_Write, LogName);
    Format_String(UART1_Write, "\n");
    xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks
  }

}

void Log_Write(const char *Line)                                  // write the Line to the log file
{ if(LogErr)                                                      // if last operation was in error
  { f_close(&LogFile);                                            // attempt to reopen the file system
    LogErr=f_mount(&FatFs, "", 0);                                // here it should quickly catch if the SD card is not there
    if(!LogErr) Log_Open();                                       // if file system OK, thne open the file
  }
  if(LogErr)                                                      // if in error: quit
  { 
    return ; }
  // fputs(Line, &LogFile);
  UINT WrLen;
  LogErr=f_write(&LogFile, Line, strlen(Line), &WrLen);   // write the message line to the log file
  if(LogErr)
  { xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
    Format_String(UART1_Write, "TaskCTRL: error when writing to "); // report write error
    Format_String(UART1_Write, LogName);
    Format_String(UART1_Write, "\n");
    xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks
  }
}

void Log_Check(void)                                      // time check:
{ if(LogErr) return;                                      // if last operation in error then don't do anything
  TickType_t OpenTime = xTaskGetTickCount()-LogOpenTime;  // when did we (re)open the log file last time
  if(OpenTime<30000) return;                              // if fresh (less than 30 seconds) then nothing to do
  f_close(&LogFile);                                      // close and reopen the log file when older than 10 seconds
  Log_Open();
}

void ProcessLog(void)                                     // process the queue of lines to be written to the log
{ char *Line;
  while(xQueueReceive(LogQueue, &Line, 0)==pdTRUE)        // get the pointer to the line from the queue
    Log_Write(Line);                                      // write the line to the log file
  Log_Check();                                            // time check the log file
}
#endif // LOG_ENABLE

#ifdef __cplusplus
  extern "C"
#endif
void vTaskCTRL(void* pvParameters)
{ 
  // UART1_Configuration(Parameters.CONbaud);

  UART1_Mutex = xSemaphoreCreateMutex();

#ifdef LOG_ENABLE
  LogQueue = xQueueCreate(8, sizeof(char *));
  LogErr=f_mount(&FatFs, "", 0);
  if(!LogErr) Log_Open();
#endif

  vTaskDelay(5);

  xSemaphoreTake(UART1_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
  Format_String(UART1_Write, "TaskCTRL: MCU ID: ");
  Format_Hex(UART1_Write, UniqueID[0]); UART1_Write(' ');
  Format_Hex(UART1_Write, UniqueID[1]); UART1_Write(' ');
  Format_Hex(UART1_Write, UniqueID[2]); UART1_Write(' ');
  Format_UnsDec(UART1_Write, getFlashSize()); Format_String(UART1_Write, "kB\n");
#ifdef LOG_ENABLE
  if(!LogErr)
  { Format_String(UART1_Write, "SD card: ");
    Format_UnsDec(UART1_Write, (uint32_t)FatFs.csize * (uint32_t)(FatFs.free_clust>>1), 4, 3 );
    Format_String(UART1_Write, "MB free\n"); }
#endif
  xSemaphoreGive(UART1_Mutex);                                  // give back UART1 to other tasks
  PrintParameters();

  NMEA.Clear();

  while(1)
  { vTaskDelay(1);

    ProcessInput();                                             // process console input
#ifdef LOG_ENABLE
    ProcessLog();                                               // process lines to written to the log file
#endif

  }
}

