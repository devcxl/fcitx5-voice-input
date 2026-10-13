#pragma once

// 测试专用假 libpipewire-0.3（issue #54 失焦保流但暂停识别回归）。
//
// 构建为独立共享库 `libpipewire-0.3.so.0`，只存在于构建目录。测试子进程通过
// LD_LIBRARY_PATH 加载它，从而在**真实**的 dlopen 边界上运行
// `PipeWireCapture::Start/Stop/SetDeliveryEnabled`——不新增生产侧重依赖注入层。
//
// 导出的 pw_* 符号使用 SDK 头文件中的精确签名（见 fake_pipewire.cpp 直接包含
// <pipewire/pipewire.h>），并额外导出本文件声明的 `fakepw_*` 控制接口。测试通过
// dlopen 该库并 dlsym 下列符号获得控制权；测试可执行文件**不链接**本库，也不链接
// 真实 libpipewire-0.3，以保证 capture 的 dlopen 路径被真实覆盖。
//
// 行为模拟：thread_loop / stream / spa_buffer 的最小模型，一个连续定时触发的
// 单线程 RT 回调（串行化，贴近真实 data-loop）。可控输入：每样本 float 值、
// 每 buffer 样本数、回调间隔、一次有界（≤2s）held-next-dequeue 屏障、注入
// stream ERROR。Stop（pw_thread_loop_stop）会释放屏障并 join RT 线程，不 detach。
//
// 该假库不接触真实 PipeWire 服务、设备或网络，也不写入任何构建目录以外的路径。

extern "C" {

// 清零所有计数与快照，把可控参数恢复默认（样本值 0、512 样本/buffer、2ms 回调）。
void fakepw_reset(void);

// 调用计数。
unsigned fakepw_stream_new_count(void);
unsigned fakepw_stream_destroy_count(void);
unsigned fakepw_stream_connect_count(void);
unsigned fakepw_stream_queue_count(void);
unsigned fakepw_core_disconnect_count(void);
unsigned fakepw_context_destroy_count(void);
unsigned fakepw_thread_loop_start_count(void);
unsigned fakepw_thread_loop_stop_count(void);
unsigned fakepw_thread_loop_destroy_count(void);
unsigned fakepw_dequeue_count(void);

// 可控输入（在 Start 前设置）。
void fakepw_set_sample_value(float value);          // 每个 float 样本写入的值
void fakepw_set_samples_per_buffer(unsigned count); // 每个 buffer 的样本数
void fakepw_set_callback_interval_ms(unsigned ms);  // 连续 RT 回调间隔
// 0=normal, 1=null dequeue, 2=no datas, 3=null chunk, 4=null data.
void fakepw_set_empty_mode(unsigned mode);
void fakepw_deliver_one_valid_buffer(void);

// 有界 held-next-dequeue 屏障：武装后，下一次 dequeue 会阻塞（快照当时的样本值），
// 直到 release 或 2s 超时。用于制造“已取 epoch 的旧 RT 回调跨快速 pause/resume”。
void fakepw_hold_next_dequeue(void);
int fakepw_dequeue_held(void);                      // 1 表示当前有 dequeue 被阻塞
void fakepw_release_held_dequeue(void);
float fakepw_held_sample_value(void);               // 阻塞时快照的旧样本值

// 注入 stream ERROR 状态回调。
void fakepw_trigger_stream_error(const char* message);

} // extern "C"
