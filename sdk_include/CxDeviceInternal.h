#ifndef CX_DEVICE_INTERNAL_H
#define CX_DEVICE_INTERNAL_H
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
CX_DEVICE_API int32_t CxDeviceSetSdkLogCallback(LogCallback func, void* userData);
CX_DEVICE_API int32_t CxDeviceGetErrorDesc(int32_t errorCode, const char** errDesc);
//设备注册
CX_DEVICE_API int32_t CxDeviceRegisterCard(CxChassisType chassisType, uint32_t deviceId, uint32_t subDeivceId, uint32_t hardwareVersion, uint32_t firmwareVersion);
//外设芯片IO操作
CX_DEVICE_API int32_t CxDeviceReadSpi(CxCardHandle cardHandle, int32_t spitype, uint32_t baseAddr, uint32_t addr, uint32_t* data);
CX_DEVICE_API int32_t CxDeviceWriteSpi(CxCardHandle cardHandle, int32_t spitype, uint32_t baseAddr, uint32_t addr, uint32_t data);
CX_DEVICE_API int32_t CxDeviceReadI2c(CxCardHandle cardHandle, int32_t i2ctype, uint32_t baseAddr, uint32_t addr, uint32_t* data);
CX_DEVICE_API int32_t CxDeviceWriteI2c(CxCardHandle cardHandle, int32_t i2ctype, uint32_t baseAddr, uint32_t addr, uint32_t data);
CX_DEVICE_API int32_t CxDeviceReadRom(CxCardHandle cardHandle, uint32_t romaddr, uint32_t* data);
CX_DEVICE_API int32_t CxDeviceWriteRom(CxCardHandle cardHandle, uint32_t romaddr, uint32_t data);
//PXIe IAP		  
CX_DEVICE_API int32_t CxDeviceGetFlashInfo(CxCardHandle cardHandle, uint32_t baseAddress, uint32_t* flashType, uint32_t* flashSizeMb);
CX_DEVICE_API int32_t CxDeviceWriteFlash128Byte(CxCardHandle cardHandle, uint32_t baseAddress, uint32_t flashWrAddress, char* data, uint32_t length);
CX_DEVICE_API int32_t CxDeviceEraseFlashOneSector(CxCardHandle cardHandle, uint32_t baseAddress, uint32_t flashEraseAddress);
CX_DEVICE_API int32_t CxDeviceReadFlash128Byte(CxCardHandle cardHandle, uint32_t baseAddress, uint32_t flashRdAddress, char* data, uint32_t length);
//读取DeviceInfo
CX_DEVICE_API int32_t CxDeviceGetChassisName(CxChassisHandle chassisHandle, const char** name);
CX_DEVICE_API int32_t CxDeviceGetChassisProperty(CxChassisHandle chassisHandle, const char* propertyName, const char** propertyValue);
CX_DEVICE_API int32_t CxDeviceGetCardName(CxCardHandle cardHandle, const char** name);
CX_DEVICE_API int32_t CxDeviceGetCardSlotNo(CxCardHandle cardHandle, uint32_t* id);
CX_DEVICE_API int32_t CxDeviceGetCardVendorId(CxCardHandle cardHandle, uint32_t* id);
CX_DEVICE_API int32_t CxDeviceGetCardDeviceId(CxCardHandle cardHandle, uint32_t* id);
CX_DEVICE_API int32_t CxDeviceGetCardSubDeviceId(CxCardHandle cardHandle, uint32_t* id);
CX_DEVICE_API int32_t CxDeviceGetCardFirmwareVersion(CxCardHandle cardHandle, uint32_t* version);
CX_DEVICE_API int32_t CxDeviceGetCardHardwareVersion(CxCardHandle cardHandle, uint32_t* version);
CX_DEVICE_API int32_t CxDeviceGetCardUUID(CxCardHandle cardHandle, const char** uuid);
CX_DEVICE_API int32_t CxDeviceGetCardChannelCount(CxCardHandle cardHandle, uint32_t* count);
CX_DEVICE_API int32_t CxDeviceGetCardProperty(CxCardHandle cardHandle, const char* propertyName, const char** propertyValue);
//卡操作
CX_DEVICE_API int32_t CxDeviceGetCardStatusValue(CxCardHandle cardHandle, const char* configName, CxStatusValueHandle value);
CX_DEVICE_API int32_t CxDeviceGetCardReadAvailable(CxCardHandle cardHandle, uint32_t* available, uint32_t* blockCount);
CX_DEVICE_API int32_t CxDeviceSetCardRecvBufferSize(CxCardHandle cardHandle, uint32_t bufferSize);
CX_DEVICE_API int32_t CxDeviceGetCardFramesCount(CxCardHandle cardHandle, uint32_t* count);
CX_DEVICE_API int32_t CxDeviceReadCardFramesDetail(CxCardHandle cardHandle, uint32_t wantedFrameCount, void** returnedFrames, uint32_t* returnedFrameCount, uint32_t* remainedFrameCount);
CX_DEVICE_API int32_t CxDeviceGetAndWaitCardFramesSpecCount(CxCardHandle cardHandle, uint32_t specCount, int32_t timeoutMilliSec, uint32_t* actualCount);
CX_DEVICE_API int32_t CxDeviceReadAndWaitCardFramesSpecDetail(CxCardHandle cardHandle, uint32_t specCount, int32_t timeoutMilliSec, void** returnedFrames, uint32_t* returnedFrameCount);
//通道操作
CX_DEVICE_API int32_t CxDeviceGetChannelStatusValue(CxCardHandle cardHandle, uint32_t chNo, const char* configName, CxStatusValueHandle value);
CX_DEVICE_API int32_t CxDeviceGetChannelReadAvailable(CxCardHandle cardHandle, uint32_t chNo, uint32_t* available, uint32_t* blockCount);
CX_DEVICE_API int32_t CxDeviceSetChannelRecvBufferSize(CxCardHandle cardHandle, uint32_t chNo, uint32_t bufferSize);
CX_DEVICE_API int32_t CxDeviceGetChannelFramesCount(CxCardHandle cardHandle, uint32_t chNo, uint32_t* count);
CX_DEVICE_API int32_t CxDeviceReadChannelFramesDetail(CxCardHandle cardHandle, uint32_t chNo, uint32_t wantedFrameCount, void** returnedFrames, uint32_t* returnedFrameCount, uint32_t* remainedFrameCount);
CX_DEVICE_API int32_t CxDeviceGetAndWaitChannelFramesSpecCount(CxCardHandle cardHandle, uint32_t chNo, uint32_t specCount, int32_t timeoutMilliSec, uint32_t* actualCount);
CX_DEVICE_API int32_t CxDeviceReadAndWaitChannelFramesSpecCount(CxCardHandle cardHandle, uint32_t chNo, uint32_t specCount, int32_t timeoutMilliSec, void** returnedFrames, uint32_t* returnedFrameCount);
//读取总线信息
CX_DEVICE_API int32_t CxDeviceGetBusIODeviceId(CxBusIOHandle baseIOHandle, uint32_t* id);
CX_DEVICE_API int32_t CxDeviceGetBusIOProperty(CxBusIOHandle baseIOHandle, const char* propertyName, const char** propertyValue);
//总线操作
CX_DEVICE_API int32_t CxDeviceGetBusIOReadAvailable(CxBusIOHandle baseIOHandle, uint32_t chNo, uint32_t* available, uint32_t* blockCount);
//存储辅助函数
CX_DEVICE_API int32_t CxDeviceInitSaveContext(CxSaveContextHandle* contextHandle);
CX_DEVICE_API int32_t CxDeviceDeinitSaveContext(CxSaveContextHandle contextHandle);
CX_DEVICE_API int32_t CxDeviceSetSaveContextOption(CxSaveContextHandle contextHandle, const char* optionName, const char* value);
CX_DEVICE_API int32_t CxDeviceOpenSaver(CxSaveContextHandle contextHandle, const char* fileName, CxDataSaverHandle* saverHandle);
CX_DEVICE_API int32_t CxDeviceGetSaverRingBuffer(CxDataSaverHandle saverHandle, CxRingBufferHandle* bufferHandle);
CX_DEVICE_API int32_t CxDeviceWriteSaverRingBuffer(CxDataSaverHandle saverHandle);
CX_DEVICE_API int32_t CxDeviceWriteSaverData(CxDataSaverHandle saverHandle, char* buffer, uint32_t offset, uint32_t saveSize);
CX_DEVICE_API int32_t CxDeviceReadSaverData(CxDataSaverHandle saverHandle, char* buffer, uint32_t offset, uint32_t readSize);
CX_DEVICE_API int32_t CxDeviceFlushSaverData(CxDataSaverHandle saverHandle);
CX_DEVICE_API int32_t CxDeviceCloseSaver(CxDataSaverHandle saverHandle);
//获取ringbuffer
CX_DEVICE_API int32_t CxDeviceInitRingBuffer(CxRingBufferHandle* bufferHandle, uint32_t bufferSize);
CX_DEVICE_API int32_t CxDeviceDeinitRingBuffer(CxRingBufferHandle bufferHandle);
CX_DEVICE_API int32_t CxDeviceGetRingBufferWriteBuffer(CxRingBufferHandle bufferHandle, char** buffer, uint32_t* continueAvailable, uint32_t* allAvailable);
CX_DEVICE_API int32_t CxDeviceCommitRingBufferWrite(CxRingBufferHandle bufferHandle, uint32_t writeSize);
CX_DEVICE_API int32_t CxDeviceGetRingBufferReadBuffer(CxRingBufferHandle bufferHandle, char** buffer, uint32_t* continueAvailable, uint32_t* allAvailable);
CX_DEVICE_API int32_t CxDeviceCommitRingBufferRead(CxRingBufferHandle bufferHandle, uint32_t readSize);
//获取MMap
CX_DEVICE_API int32_t CxDeviceInitMMapWriter(const char* mmapName, CxMMapWriterHandle* writerHandle, uint32_t bufferSize, bool isBlock);
CX_DEVICE_API int32_t CxDeviceDeinitMMapWriter(CxMMapWriterHandle writerHandle);
CX_DEVICE_API int32_t CxDeviceGetMMapWriteBuffer(CxMMapWriterHandle writerHandle, char** buffer, uint32_t* continueAvailable, uint32_t* allAvailable);
CX_DEVICE_API int32_t CxDeviceCommitMMapWrite(CxMMapWriterHandle writerHandle, uint32_t writeSize);
CX_DEVICE_API int32_t CxDeviceInitMMapReader(const char* mmapName, CxMMapReaderHandle* readerHandle, bool isBlock);
CX_DEVICE_API int32_t CxDeviceDeinitMMapReader(CxMMapReaderHandle readerHandle);
CX_DEVICE_API int32_t CxDeviceGetMMapReadBuffer(CxMMapReaderHandle readerHandle, char** buffer, uint32_t* continueAvailable, uint32_t* allAvailable);
CX_DEVICE_API int32_t CxDeviceCommitMMapRead(CxMMapReaderHandle readerHandle, uint32_t readSize);
#endif
