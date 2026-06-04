// CxDeviceCore.h: 标准系统包含文件的包含文件
#ifndef CX_DEVICE_CORE_H
#define CX_DEVICE_CORE_H
#include "CxDefine.h"
//windows
#if (defined(_WIN32) || defined(_WIN64))
#include <windows.h>
#ifdef CXDEVICE_EXPORTS
#define CX_DEVICE_API extern "C" __declspec(dllexport)
#else
#define CX_DEVICE_API extern "C" __declspec(dllimport)
#endif  
#define CALLBACK __stdcall
//linux
#elif defined(__linux__) || defined(__APPLE__)
#define CALLBACK  
#define CX_DEVICE_API extern "C"
#ifdef __OBJC__
#include "objc/objc.h"
#endif
#else
#error "Unsupported platform"
#endif
//sdk oprs;
CX_DEVICE_API int32_t CxDeviceSetSdkOption(const char* optionName, const char* value);
CX_DEVICE_API int32_t CxDeviceGetSdkOption(const char* optionName, const char** value);
CX_DEVICE_API int32_t CxDeviceSdkInit();
CX_DEVICE_API int32_t CxDeviceSdkDeinit();
//chassis oprs; support one or multi chassis
CX_DEVICE_API int32_t CxDeviceGetChassis(CxChassisType type, CxChassisHandle* chassisHandle);
CX_DEVICE_API int32_t CxDeviceReleaseChassis(CxChassisHandle chassisHandle);
CX_DEVICE_API int32_t CxDeviceSetChassisOption(CxChassisHandle chassisHandle, const char* optionName, const char* value);
CX_DEVICE_API int32_t CxDeviceGetChassisOption(CxChassisHandle chassisHandle, const char* optionName, const char** value);
CX_DEVICE_API int32_t CxDeviceGetCards(CxChassisHandle chassisHandle, CxCardHandle** cardHandles, uint32_t* cardCount);
//card oprs;
CX_DEVICE_API int32_t CxDeviceSetCard(CxCardHandle cardHandle, const char* configName);
CX_DEVICE_API int32_t CxDeviceSetCardValue(CxCardHandle cardHandle, const char* configName, const char* value);
CX_DEVICE_API int32_t CxDeviceGetCardStatus(CxCardHandle cardHandle, const char* configName, const char** value);
CX_DEVICE_API int32_t CxDeviceWriteReg(CxCardHandle cardHandle, uint32_t addr, uint32_t data);
CX_DEVICE_API int32_t CxDeviceReadReg(CxCardHandle cardHandle, uint32_t addr, uint32_t* data);
CX_DEVICE_API int32_t CxDeviceReadCardFrames(CxCardHandle cardHandle, void** frames, uint32_t* frameCount);
CX_DEVICE_API int32_t CxDeviceSetCardRecvCallback(CxCardHandle cardHandle, OnePackComeCallback packCome, void* userData);
CX_DEVICE_API int32_t CxDeviceStartCardRecv(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceStopCardRecv(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceStartSaveCardData(CxCardHandle cardHandle, const char* fileName, uint32_t bufSize);
CX_DEVICE_API int32_t CxDeviceStopSaveCardData(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceWriteCardRawData(CxCardHandle cardHandle, char* userDefineBuffer,
    uint32_t writeLen, uint32_t* actualWriteLen, uint32_t timeoutMilliSec);
CX_DEVICE_API int32_t CxDeviceWriteCardRawDataOffset(CxCardHandle cardHandle, char* userDefineBuffer, uint32_t offset,
    uint32_t writeLen, uint32_t* actualWriteLen, uint32_t timeoutMilliSec);
CX_DEVICE_API int32_t CxDeviceReadCardRawData(CxCardHandle cardHandle, char* userDefineBuffer,
    uint32_t wantLen, uint32_t* actualWriteLen, uint32_t timeoutMilliSec);
CX_DEVICE_API int32_t CxDeviceReadCardRawDataOffset(CxCardHandle cardHandle, char* userDefineBuffer, uint32_t offset,
    uint32_t wantLen, uint32_t* actualReadLen, uint32_t timeoutMilliSec);
//channel oprs;
CX_DEVICE_API int32_t CxDeviceSetChannel(CxCardHandle cardHandle, uint32_t chNo, const char* configName);
CX_DEVICE_API int32_t CxDeviceSetChannelValue(CxCardHandle cardHandle, uint32_t chNo, const char* configName, const char* value);
CX_DEVICE_API int32_t CxDeviceGetChannelStatus(CxCardHandle cardHandle, uint32_t chNo, const char* configName, const char** value);
CX_DEVICE_API int32_t CxDeviceReadChannelFrames(CxCardHandle cardHandle, uint32_t chNo, void** frames, uint32_t* frameCount);
CX_DEVICE_API int32_t CxDeviceSetChannelRecvCallback(CxCardHandle cardHandle, uint32_t chNo, OnePackComeCallback packCome, void* userData);
CX_DEVICE_API int32_t CxDeviceStartChannelRecv(CxCardHandle cardHandle, uint32_t chNo);
CX_DEVICE_API int32_t CxDeviceStopChannelRecv(CxCardHandle cardHandle, uint32_t chNo);
CX_DEVICE_API int32_t CxDeviceStartSaveChannelData(CxCardHandle cardHandle, uint32_t chNo, const char* fileName, uint32_t bufSize);
CX_DEVICE_API int32_t CxDeviceStopSaveChannelData(CxCardHandle cardHandle, uint32_t chNo);
CX_DEVICE_API int32_t CxDeviceWriteChannelRawData(CxCardHandle cardHandle, uint32_t chNo, char* userDefineBuffer,
    uint32_t writeLen, uint32_t* actualWriteLen, uint32_t timeoutMilliSec);
CX_DEVICE_API int32_t CxDeviceWriteChannelRawDataOffset(CxCardHandle cardHandle, uint32_t chNo, char* userDefineBuffer, uint32_t offset,
    uint32_t writeLen, uint32_t* actualWriteLen, uint32_t timeoutMilliSec);
CX_DEVICE_API int32_t CxDeviceReadChannelRawData(CxCardHandle cardHandle, uint32_t chNo, char* userDefineBuffer,
    uint32_t wantLen, uint32_t* actualReadLen, uint32_t timeoutMilliSec);
CX_DEVICE_API int32_t CxDeviceReadChannelRawDataOffset(CxCardHandle cardHandle, uint32_t chNo, char* userDefineBuffer,
    uint32_t offset, uint32_t wantLen, uint32_t* actualReadLen, uint32_t timeoutMilliSec);
//his;card oprs
CX_DEVICE_API int32_t CxDeviceOpenCard(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceCloseCard(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceInitCard(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceWriteCardConfig(CxCardHandle cardHandle, uint32_t type, int32_t data);
CX_DEVICE_API int32_t CxDeviceResetCard(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceEnableCard(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceDisableCard(CxCardHandle cardHandle);
CX_DEVICE_API int32_t CxDeviceCardSoftwareTrigger(CxCardHandle cardHandle);
//his;channel oprs old
CX_DEVICE_API int32_t CxDeviceWriteChannelConfig(CxCardHandle cardHandle, uint32_t chNo, uint32_t type, int32_t data);
CX_DEVICE_API int32_t CxDeviceEnableChannel(CxCardHandle cardHandle, uint32_t chNo);
CX_DEVICE_API int32_t CxDeviceDisableChannel(CxCardHandle cardHandle, uint32_t chNo);
CX_DEVICE_API int32_t CxDeviceChannelSoftwareTrigger(CxCardHandle cardHandle, uint32_t chNo);
#endif
