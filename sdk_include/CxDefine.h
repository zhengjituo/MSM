#ifndef CX_DEFINE_H
#define CX_DEFINE_H
#include <stdint.h>
//windows
#if (defined(_WIN32) || defined(_WIN64)) 
#define CALLBACK __stdcall
//linux
#elif defined(__linux__) || defined(__APPLE__)
#define CALLBACK
#ifdef __OBJC__
#include "objc/objc.h"
#endif
#else
#error "Unsupported platform"
#endif
// 用户设备handle
typedef void* CxChassisHandle;
typedef void* CxCardHandle;
typedef void* CxChannelHandle;
typedef void* CxBusDriverHandle;
typedef void* CxBusIOHandle;
// Status值类型
typedef void* CxStatusValueHandle;
// 数据包handle
typedef void* CxPackHandle;
// 存储
typedef void* CxSaveContextHandle;
typedef void* CxDataSaverHandle;
// ringbuffer
typedef void* CxRingBufferHandle;
// MMap
typedef void* CxMMapWriterHandle;
typedef void* CxMMapReaderHandle;
//机箱类型
enum CxChassisType
{
    PXIe,
    PCIeMiuXI,
    USBMiuXI,
    NetTrans
};
//总线类型
enum CxBusType
{
    PCIe,
    USB,
    TCP,
    VirtualMiuXI, // miuxi 虚拟总线
    VirtualTrans, // 通用转发平台 虚拟总线
};
//功能配置
typedef enum
{
    /******通用功能，以及历史遗留*******/
    /******From 0x00000000**************/
    /******End  0x00200000**************/
    /*元器件电路配置*/
    ClockSource                            = 0x00000001,        // 时钟源
    CouplingMode                           = 0x00000002,        // 耦合方式：直流、交流
    InputImpedance                         = 0x00000003,        // 输入阻抗，单位欧姆
    PreamplifierGain                       = 0x00000004,        // 前放增益
    SignalEnable                           = 0x00000005,        // 信号输入使能
    VoltageOffset                          = 0x00000006,        // 偏置电压，单位mV
    VGAGain                                = 0x00000007,        // VGA增益
    VoltageRange                           = 0x00000008,        // 电压量程，单位mV
    CurrentRange                           = 0x00000009,        // 电流量程，单位mA
    /*触发相关*/
    TrigSource                             = 0x0000000A,        // 触发源
    FiniteTrigNum                          = 0x0000000B,        // 有限次触发次数
    AutoTrigPeriod                         = 0x0000000C,        // 通道自触发周期，单位ns
    TrigMode                               = 0x0000000D,        // 触发模式
    TrigPolarity                           = 0x0000000E,        // 触发极性
    TrigLevel                              = 0x0000000F,        // 触发电平，单位mV
    ExternalTrigPolarity                   = 0x00000010,        // 外触发极性
    TrigPosition                           = 0x00000011,        // 触发位置，单位ns
    TrigLength                             = 0x00000012,        // 触发长度，单位ns
    TrigPulseWidth                         = 0x00000013,        // 触发脉宽，单位ns
    TrigDelayTime                          = 0x00000014,        // 触发延迟，单位ns
    ExternalTrigLevel                      = 0x00000015,        // 外部触发阈值，单位mV
    MultiTrigEnable                        = 0x00000016,        // 多重触发使能（触发内的触发）
    TrigPoints                             = 0x00000017,        // 触发点数
    SoftwareTrig                           = 0x00100018,        // 软触发
    FrameAddNumber                         = 0x00000019,        // 累加次数
    FrameAddEnable                         = 0x0000001A,        // 累加使能
    TrigPositionPoints                     = 0x0000001B,        // 触发位置点数
    DeadTime                               = 0x0000001C,        // 死时间
    DownSampleCoef                         = 0x0000001D,        // 降采样系数
    WaveInversion                          = 0x0000001E,        // 波形反相
    PackageLength                          = 0x0000001F,        // 包长度
    PackageMode                            = 0x00000020,        // 采集模式
    /*恒比定时PSD等业务*/
    CFDLatency                             = 0x00000021,        // 恒比定时延迟
    CFDAttenuation                         = 0x00000022,        // 恒比定时衰减
    GateOffset                             = 0x00000023,        // 门偏置，单位ns
    ShortGate                              = 0x00000024,        // 短门，单位ns
    LongGate                               = 0x00000025,        // 长门，单位ns
    GateOffsetMode                         = 0x00000026,        // 门偏置模式
    /*全局同步和触发-采集卡和控制卡*/
    SyncWindowEnable                       = 0x00000027,        // 采集卡同步开窗使能（同步采集开窗，多通道采集用，时间计数重新刷新，从0开始）
    SyncWindowLength                       = 0x00000028,        // 采集卡同步开窗长度，单位ns
	SyncSource                             = 0x00000029,        // 采集卡同步源 0: 背板全局 1: 前面板本地
    SyncPosition                           = 0x0000002A,        // 采集卡开窗的位置，单位ns
	SyncPeriod                             = 0x0000002B,        // 采集卡同步周期
	SyncDeadTime                           = 0x0000002C,        // 采集卡同步死时间，单位ns
	SoftwareSync                           = 0x0000002D,        // 采集卡软件同步
    TrigWindowStart                        = 0x0000002E,        // 采集卡触发窗口开始，单位ns（每个通道符合开窗范围，通道用户自定义，相对于SyncWindow的0点）
    TrigWindowEnd                          = 0x0000002F,        // 采集卡触发窗口结束，单位ns（相对于SyncWindow的0点）
    GlobalTrigEnable                       = 0x00000030,        // 采集卡全局触发使能通道，参数为通道号
    GlobalTrigDisable                      = 0x00000031,        // 采集卡全局触发失能通道，参数为通道号
    GlobalTrigMode                         = 0x00000032,        // 采集卡全局触发与或逻辑，1表示与逻辑，0表示或逻辑
    //GlobalTrigMode                                            // 控制卡全局触发与或逻辑，1表示与逻辑，0表示或逻辑
	//TrigSource                                                // 控制卡触发源 1:通道触发源 2:外部触发源 4:用户触发源
    //SoftwareTrig                                              // 控制卡软触发
	//SyncSource                                                // 控制卡同步源 0:强制同步 1:周期同步 2:外部同步
    //SoftwareSync                                              // 控制卡软同步
	GlobalResetDelay                       = 0x00000033,        // 控制卡全局复位延时，单位ns
    /*基线跟踪*/
    TrigBaselineMode                       = 0x00000034,        // 基线模式
    TraceBaselineWindow                    = 0x00000035,        // 基线跟踪长度，单位ns
    TraceBaselineBeforeTrig                = 0x00000036,        // 基线跟踪前置偏移，单位ns
    FixedBaseLineLevel                     = 0x00000037,        // 固定基线阈值，单位mV
    RawDataEnable                          = 0x00000038,        // 波形数据使能
    /*定制化*/
    TimeNHitThreshold                      = 0x00000039,        // 设置TCM数字触发匹配窗
    TimeTrigWindow                         = 0x0000003A,        // 设置TCM总数字触发参数
    TimeT0FrequencyDown                    = 0x0000003B,        // 设置T0降频参数
    T0Mode                                 = 0x0000003C,        // 设置T0信号来源
    T0FakeCycle                            = 0x0000003D,        // 设置模拟T0产生频次
    T0MeasureEnable                        = 0x0000003E,        // 设置T0_measure使能
    LoadTemperature1Data                   = 0x0000003F,        // 读取温度传感器1数值，单位为0.01摄氏度
    LoadTemperature2Data                   = 0x00000040,        // 读取温度传感器2数值，单位为0.01摄氏度
    BoardFitIPChannelAcquisition           = 0x00000041,        // 本地板卡触发获取通道
    BoardFitIPChannelAntiAcquisition       = 0x00000042,        // 本地板卡触发反符合通道
    BoardFitIPAntiAcquisitionWindowBefore  = 0x00000043,        // 本地板卡触发前反符合时间，单位ns
    BoardFitIPAntiAcquisitionWindow        = 0x00000044,        // 本地板卡触发后反符合时间，单位ns
    TCMSoftTrig                            = 0x00000045,
    CardSynMode                            = 0x00000046,
    TCMTrigEnable                          = 0x00000047,
    TCMTrigSourceSel                       = 0x00000048,
    /*Adc校准*/
    AdcCaliEnable                          = 0x00000049,
    AdcDataPolarReserveEn                  = 0x0000004A,
    AdcCaliFactor                          = 0x0000004B,
    /******核电子学专用IP*******/
    /******From 0x00200000******/
    /******End  0x00300000******/
    PHAEnable                              = 0x00200000,
    PHAParamK                              = 0x00200001,        // 梯形上升时间
    PHAParamL                              = 0x00200002,        // 梯形平顶 + 上升时间
    PHAParamM                              = 0x00200003,        // 滤波系数
    PHAPeakThresholdLow32Bit               = 0x00200004,        // 三角过阈低32位
    PHAPeakThresholdHigh32Bit              = 0x00200005,        // 三角高阈低32位
    PHAPeakThresholdCount                  = 0x00200016,        // 过阈宽度
    PHAADCOTR                              = 0x00200006,
    PHAPeakExtDelayTime                    = 0x00200007,        // 峰值延迟时间
    TTFilterEnable                         = 0x00200008,
    TTFilterThreshold                      = 0x00200009,        // 过阈值
    TTFilterParam1                         = 0x0020000A,
    TTFilterParam2                         = 0x0020000B,
    TTFilterParam3                         = 0x0020000C,
    CFDEnable                              = 0x0020000D,
    CFDParamK                              = 0x0020000E,
    CFDDelayLen                            = 0x0020000F,
    CFDAmpThreshold                        = 0x00200010,
    CFDPolar                               = 0x00200011,
    PSDEnable                              = 0x00200012,
    PSDPreGate                             = 0x00200013,        // 开门位置
    PSDShortGateLen                        = 0x00200014,        // 短门长度
    PSDLongGateLen                         = 0x00200015,        // 长门长度
    /******多物理量测量专用IP*****/
    /******From 0x00300000*****/
    /******End  0x00400000*****/
    /*通道使能、采集启停相关*/
    MDAQGlobalAcqEnable                    = 0x00300001,        // 板卡采集使能
    MDAQChannelAcqEnable                   = 0x00300002,        // 通道采集使能
    MDAQChannelEnable                      = 0x00300003,        // 通道数据使能(通道打包使能)
    MDAQSampleRate                         = 0x00300003,        // 采样率（下采样采样率）
    MDAQADCSampleRate                      = 0x00300004,        // ADC采样率
    /*测量模式相关*/
    MDAQMeasureType                        = 0x00300100,        // 测量类型切换：执行界面配置项以外的默认配置
    MDAQMeasureRangeType                   = 0x00300101,        // 量程（电压、电流、电阻、电位器、温度、IEPE、电桥、计数器、CAN）
    MDAQSensorType                         = 0x00300102,        // RTD传感器类型
    MDAQBridgeMode                         = 0x00300103,        // 桥模式
    MDAQBridgeResistor                     = 0x00300104,        // 桥电阻
    /*激励输出相关*/
    MDAQExcitationSource                   = 0x00300200,        // 激励源选择
    MDAQExcitationVoltage                  = 0x00300201,        // 激励电压输出
    MDAQExcitationCurrent                  = 0x00300202,        // 激励电流输出
    MDAQIEPECurrent                        = 0x00300203,        // IEPE激励电流
    /*信号处理参数相关*/
    MDAQAnalogFilterFrequency              = 0x00300300,        // 模拟滤波器频率
    MDAQDigitalLPIIRFilterType             = 0x00300301,        // 数字滤波器类型
    MDAQDigitalLPIIRFilterOrder            = 0x00300302,        // 数字滤波器阶数
    MDAQDigitalLPIIRFilterFrequency        = 0x00300303,        // 数字滤波器频率
    /*计数器参数相关*/
    MDAQCounterEnable                      = 0x00300401,        // 计数使能
    MDAQCounterFilter                      = 0x00300402,        // 计数器门限滤波
    MDAQCounterInvert                      = 0x00300403,        // 取反
    MDAQCounterMode                        = 0x00300404,        // 计数模式
    MDAQCounterChannel                     = 0x00300405,        // 计数通道
    MDAQCounterSampleRate                  = 0x00300406,        // 采样率
    MDAQCounterReset                       = 0x00300407,        // 复位
    MDAQCounterHardwareReset               = 0x00300408,        // 硬件复位
    MDAQCounterTrigThreshold               = 0x00300409,        // 触发阈值
    MDAQCounterRetrigThreshold             = 0x0030040A,        // 重触发阈值
    MDAQCounterSourceACouplingMode         = 0x0030040B,        // 输入A交直流耦合
    MDAQCounterSourceAInputType            = 0x0030040C,        // 输入A数字/模拟输入
    MDAQCounterSourceBInputType            = 0x0030040D,        // 输入B数字/模拟输入
    MDAQCounterSourceZInputType            = 0x0030040E,        // 输入Z数字/模拟输入
    /******测试测量专用IP*****/
    /******From 0x00400000*****/
    /******End  0x00500000*****/
    /*PCIE_X1073_V3_0板卡 波形重建*/
    WFRebuildEnable                        = 0x00400004,        // 波形累加使能
    WFRebuildPresetBaseLine                = 0x00400005,        // 波形重构预设基线值
    WFRebuildWidthThreshold                = 0x00400006,        // 脉宽筛选阈值
    WFRebuildValueThreshold                = 0x00400007,        // 峰值有效筛选
    WFRebuildPolar                         = 0x00400008,        // 极性
    /* TriggerOut相关 */
    TrigOutFreq                            = 0x00400015,        // 自生成波形频率
    TrigOutWidth                           = 0x00400016,        // 脉宽筛选阈值
    TrigOutMode                            = 0x00400017,        // 工作模式
    TrigOutEnable                          = 0x00400018,        // 使能
    /******其他定制化专用IP*****/
    /******From 0x00E00000*****/
    /******End  *****/
    /*积分处理-1012定制*/
    IntegralDataMode                       = 0x00E00000,        // 数据模式
    IntegralWindowLow                      = 0x00E00001,        // 区间积分窗口下限
    IntegralWindowHigh                     = 0x00E00002,        // 区间积分窗口上限
    IntegralSumNumber                      = 0x00E00003,        // 区间积分累加次数(指数)
    IntegralLogicNoiseFilterEnable         = 0x00E00004,        // 本底噪声去除使能
    IntegralLocalNoiseNumber               = 0x00E00005,        // 本底噪声计算区间上限
    /*SyncAndTrig 3010定制*/
    SyncTrigIpReset                        = 0x00E00010,
    SyncTrigPathReset                      = 0x00E00011,
    SyncTrigGlbSync                        = 0x00E00012,
    SyncTrigTrigMux                        = 0x00E00013,
    SyncTrigSoftTrig                       = 0x00E00014,
    /*DelaySysRst 3010定制*/
    DelaySysIpRest                         = 0x00E00020,
    DelaySysPathRest                       = 0x00E00021,
    DelaySysDelayValue                     = 0x00E00022,
    /*TrigOut3010M1 3010定制*/
    TrigOut3010IpReset                     = 0x00E00030,
    TrigOut3010PathReset                   = 0x00E00031,
    TrigOut3010TrigEnable                  = 0x00E00032,
    TrigOut3010TrigMode                    = 0x00E00033,
    /*TdcManager*/
    TdcStart                               = 0x00E00040,
    TdcTrigLatency                         = 0x00E00041,
    TdcMatchWindow                         = 0x00E00042
}CONFIG_ID;
/// <summary>
/// 原始数据包，包头指针和长度
/// </summary>
typedef struct RawPack
{
    char* Buffer;
    uint32_t BufferSize;
}RawPack;
/// <summary>
/// 采集数据包
/// </summary>
typedef struct SciAcqPackage
{
    // stable
    /// <summary>
    /// 包类型
    /// </summary>
    uint8_t PackType;
    /// <summary>
    /// 板载类型
    /// </summary>
    uint32_t BoardType;
    /// <summary>
    /// 采样频率
    /// </summary>
    uint16_t SampleFrequenceMHz;
    /// <summary>
    /// 采样分辨率
    /// </summary>
    uint8_t Resolution;
    /// <summary>
    /// 通道号
    /// </summary>
    uint16_t ChannelId;
    /// <summary>
    /// 槽位号
    /// </summary>
    uint8_t SlotId;
    /// <summary>
    /// 固件版本
    /// </summary>
    //uint32_t FirmwareVersion;
    // var change 
    /// <summary>
    /// 触发号
    /// </summary>
    uint32_t TriggerId;
    /// <summary>
    /// 秒数，相对于utc的时间
    /// </summary>
    uint64_t TimestampSecond;
    /// <summary>
    /// 时间-纳秒，秒以下的计量
    /// </summary>
    uint32_t TimestampNanosecond;
    /// <summary>
    /// 8bit 补偿精细时间戳，单位根据板卡型号有所区别，例60ps
    /// </summary>
    uint8_t TimestampFine;
    /// <summary>
    /// 波形码值偏移
    /// </summary>
    int32_t ADCDigitOffset;
    /// <summary>
    /// 波形增益
    /// </summary>
    double ADCPreGain;
    /// <summary>
    /// 量程 mv
    /// </summary>
    int32_t ADCRange;
    /// <summary>
    /// ADC 精度位数
    /// </summary>
    uint8_t ADCPrecision;
    /// <summary>
    /// 波形解析方式
    /// 0 - WaveData 中连续两字节大端，
    /// 1 - 连续两字节小端， 
    /// ......
    /// 一个点的实际电压 mv = （WaveData 一个解码点 - ADCDigitOffset）* ADCRange * ADCPregain / (1 << ADCPrecision)
    /// </summary>
    uint8_t DecodeMode;
    /// <summary>
    /// 波形数据长度
    /// </summary>
    int32_t WaveDataSize;
    /// <summary>
    /// 波形数据
    /// 由SDK 维护的内存，
    /// 请尽快copy走本块内存，然后处理
    /// </summary>
    char* WaveData;
}SciAcqPackage;
/// <summary>
/// 一个包到达的回调
/// </summary>
typedef void (CALLBACK* OnePackComeCallback)(void* package, uint32_t length, void* userData);
//日志类型
typedef enum
{
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    UnknowLevel
}CxLogLevel;
/// <summary>
/// 日志回调
/// </summary>
typedef void (CALLBACK* LogCallback)(uint32_t level, const char* data, void* userData);
//时钟源选项
typedef enum
{
    OnBoardClock = 0,       // 板载时钟
    DSTARAClock = 1,        // 背板时钟
    ExternalClock = 2,      // 外部时钟
    ExternalClock100M = 3   // 外部时钟100M
}ClockSourceEnum;
//触发源选项
typedef enum
{
    LocalChannel = 0b1, // 本地通道
    LocalBoard = 0b10,  // 本地板卡
    Global = 0b100,     // 系统触发
    External = 0b1000,  // 外部触发
    Force = 0b10000,    // 强制触发
    Cycle = 0b100000    // 周期触发
}TriggerSourceEnum;
//触发模式选项
typedef enum
{
    Finite = 0b1,     // 有限
    Normal = 0b10,    // 普通
    AutoMode = 0b100  // 自动
}TriggerModeEnum;
//触发极性选项
typedef enum
{
    Falling = 0, // 下降沿
    Rising = 1   // 上升沿
}TriggerPolarityEnum;
//耦合方式选项
typedef enum
{
    ACCoupling = 0, // 交流耦合
    DCCoupling = 1  // 直流耦合
}CouplingModeEnum;
//输入阻抗选项
typedef enum
{
    Load50 = 0, // 50欧姆
    Load1M = 1  // 1兆欧姆
}InputImpedanceEnum;
//基线模式选项
typedef enum
{
    Fixed = 0,  // 固定
    TraceB = 1  // 自动调节
}BaselineModeEnum;
#endif