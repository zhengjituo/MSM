#ifndef CXDEVICE_BUS_H
#define CXDEVICE_BUS_H
#include <stdint.h>
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
CX_DEVICE_API int32_t CxDeviceSetBusSDKOption(const char* optionName, const char* value);
CX_DEVICE_API int32_t CxDeviceGetBusDriver(CxBusType type, CxBusDriverHandle* driverHandle);
CX_DEVICE_API int32_t CxDeviceSetBusDriverOption(CxBusDriverHandle busDriverHandle, const char* optionName, const char* value);
CX_DEVICE_API int32_t CxDeviceGetBusIOs(CxBusDriverHandle busDriverHandle, CxBusIOHandle** baseIOHandles, uint32_t* count);
CX_DEVICE_API int32_t CxDeviceSetBusIOOption(CxBusIOHandle baseIOHandle, const char* optionName, const char* value);
CX_DEVICE_API int32_t CxDeviceOpenBus(CxBusIOHandle baseIOHandle);
CX_DEVICE_API int32_t CxDeviceCloseBus(CxBusIOHandle baseIOHandle);
CX_DEVICE_API int32_t CxDeviceReadBus(CxBusIOHandle baseIOHandle, uint32_t pipeNo, char* buffer, uint32_t requireSize, uint32_t* actualSize);
CX_DEVICE_API int32_t CxDeviceReadBusOffset(CxBusIOHandle baseIOHandle, uint32_t pipeNo, char* buffer, uint32_t offset, uint32_t requireSize, uint32_t* actualSize);
CX_DEVICE_API int32_t CxDeviceWriteBus(CxBusIOHandle baseIOHandle, uint32_t pipeNo, char* buffer, uint32_t requireSize, uint32_t* actualSize);
CX_DEVICE_API int32_t CxDeviceWriteBusOffset(CxBusIOHandle baseIOHandle, uint32_t pipeNo, char* buffer, uint32_t offset, uint32_t requireSize, uint32_t* actualSize);
CX_DEVICE_API int32_t CxDeviceWriteBusReg(CxBusIOHandle baseIOHandle, uint32_t addr, uint32_t data);
CX_DEVICE_API int32_t CxDeviceReadBusReg(CxBusIOHandle baseIOHandle, uint32_t addr, uint32_t* data);
#endif