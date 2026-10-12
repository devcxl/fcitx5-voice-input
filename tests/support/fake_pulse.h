#pragma once

// 测试专用假 libpulse-simple（issue #54 录音源误选回归）。
//
// 构建为独立共享库 `libpulse-simple.so.0`，只存在于构建目录。测试子进程通过
// LD_LIBRARY_PATH 加载它，从而在**真实**的 dlopen 边界上运行
// `PulseAudioCapture::Start/Stop`——不新增生产侧重依赖注入层。
//
// 导出的 pa_* 符号使用 SDK 头文件中的精确签名（见 fake_pulse.cpp 直接包含
// <pulse/simple.h> 定义），并额外导出本文件声明的 `fakepulse_*` 控制接口。
// 测试通过 dlopen 该库并 dlsym 下列符号获得控制权；测试可执行文件**不链接**
// 本库，以保证 capture 的 dlopen 路径被真实覆盖。
//
// 记录内容：new/free/read 调用计数，建流参数快照（dev 是否为 NULL、
// 应用名、流名、方向、sample spec）。可控输入：建流失败、每次 read 返回的
// int16 采样值、read 返回前的延迟（保证 Stop 的 join 有界）。
//
// 该假库不接触真实音频服务、设备或网络；仅可写入测试指定路径的标记文件
// （由假 pactl 使用，见 tests/CMakeLists.txt）。可被后续 pipeline 测试复用。

#include <pulse/def.h>

extern "C" {

// 清零所有计数与快照，并把可控参数恢复为默认（read 延迟 2ms、采样值 0、建流成功）。
void fakepulse_reset(void);

// 调用计数。
unsigned fakepulse_new_count(void);
unsigned fakepulse_free_count(void);
unsigned fakepulse_read_count(void);

// 最近一次 pa_simple_new 的参数快照。
int fakepulse_dev_is_null(void);              // 1 表示 dev 参数为 NULL（委托路由）
const char* fakepulse_last_dev(void);         // dev 文本，NULL 时为空串
const char* fakepulse_app_name(void);         // name 参数
const char* fakepulse_stream_name(void);      // stream_name 参数
int fakepulse_stream_direction(void);         // pa_stream_direction_t
int fakepulse_have_sample_spec(void);         // 1 表示 ss 非 NULL
int fakepulse_sample_format(void);            // pa_sample_format_t
unsigned fakepulse_sample_rate(void);         // ss->rate
unsigned fakepulse_channels(void);            // ss->channels

// 可控输入。
void fakepulse_set_fail_new(int fail);        // 非 0 时 pa_simple_new 返回 NULL
void fakepulse_set_fail_read(int fail);
void fakepulse_set_read_sample_value(int value);  // 每次 read 写入的 int16 值
void fakepulse_set_read_delay_ms(unsigned ms);    // 每次 read 返回前的延迟

} // extern "C"
