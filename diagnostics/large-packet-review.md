# 无线 CMSIS-DAP 大包验证（2026-10-06）

目标：验证将无线 DAP 命令容量由 512 扩大到 1024 字节能否改善 Keil 下载。

## 实现与边界

- 新增 CONFIG_DAP_USBIP_PACKET_SIZE；1024 已完成探针自测，但 Keil 下载失败，
  因此当前默认值和烧录配置恢复为 512。2048 尚未验收。
- USB HS bulk 描述符的 wMaxPacketSize 保持 512。DAP_Info(FF) 对无线返回
  1024；物理 USB bulk/HID 仍使用 512。VID/PID、接口与端点拓扑不变。
- 扩大无线响应缓存、核间队列与 DAP 核处理缓存，DAP 栈改为 6144 字节；
  协议处理把响应上限限制到请求来源的容量，防止物理 USB 获得过大响应。
- USB/IP 以 URB 为请求边界，一次 URB 可以包含多个物理 USB packet。
  不应直接把端点描述符改成 1024，USB 2.0 HS bulk 不支持该端点包长。
- 1024B 最大块写入：5B 命令头 + 254*4B 数据 = 1021B；有效数据 1016B。
  最大块读取响应：4B 响应头 + 255*4B 数据 = 1024B。
- 2048B 理论块写入有效载荷 2040B，读取有效载荷 2044B；主机是否使用、
  内存开销与实际下载收益都需要另行测试，不能根据容量推断翻倍提速。

## 已验证

构建和链接后 USB 描述符检查通过；已通过 COM6 烧录，Windows WinUSB 挂载正常。
wireless_probe.py --target 结果见 results/wireless-winusb-1024.json：

- DAP_Info 返回 1024/8。
- 完整 1024B OUT 查询；1023B Transfer 请求（只更新探针匹配掩码，无目标写入）。
- 255 次 DPIDR 块读的完整 1024B IN 响应，逐个值校验正确。
- 超出容量一个字的块读返回协议错误，无截断或越界。
- 20 轮 8 包排队、先 IN 后 OUT、取消重试、目标 CPUID 读取均通过。

## 等量只读基准

large_packet_benchmark.py 对比 2 次 127 字块读与 1 次 254 字块读，均读取
1016 字节 DPIDR。每种 20 轮，交替先后顺序，结果见
results/large-packet-benchmark.json。未写目标内存或 Flash。

| 相同有效数据量 | 中位数 | P95 |
|---|---:|---:|
| 两个小命令 | 45.310 ms | 107.944 ms |
| 一个大命令 | 28.314 ms | 43.940 ms |

中位耗时减少约 38%，这是自有客户端合并命令的收益，**不是 Keil 下载提速测量**。
此前 Keil 下载约 82% 为 <=64B 小请求；即使块数据命令变大，控制/轮询请求
也未必减少，所以不能推断整次下载减半。

## Keil 验收

用户用原 Keil 工程下载时报告 `Cannot Load Flash Programming Algorithm!`。
results/large-packet-uart.log 确认 Keil 曾发送 1021B 的 OUT 请求，随后反复重连；
因此 1024B 版本不能用于 Keil 下载。第一轮日志未记录 IN URB 长度，无法仅凭
该日志断定是响应截断、目标 RAM 块写入还是其他原因。后续诊断固件增加 IN 请求/
响应最大长度、截短次数和 Transfer 错误计数，但其日志中唯一错误来自第二个
Keil 实例的连接尝试，没有复现原来的 Flash 算法加载失败，不能据此归因。
现阶段结论是 **1024B 只通过自有客户端测试，没有通过 Keil 实际下载**。

## 回退

原可用 512B 固件和配置备份在 before-large-packet/。当前配置已恢复 512；
已重新构建、通过链接后的描述符检查、经 COM6 烧录并校验成功。
无线重新挂载后 WinUSB 回归通过：容量 512、队列 8，20 轮排队、满尺寸
OUT/IN、先 IN 后 OUT、取消后重试、超限拒绝均通过；DPIDR=0x6ba02477，
CPUID=0x411fc271。结果见 results/wireless-winusb.json。
启动日志为 results/rollback-512-uart.log。用户已用原 Keil 工程确认“可以下载”。
results/rollback-512-keil-uart.log 捕获的统计窗口中 OUT 最大 509B，IN 请求最大
512B，实际响应最大 508B，截短次数 0、Transfer 错误 0。该窗口不是完整下载
计时，不能用于推断速度变化。COM6 抓取已停止并释放串口。

结论：512B 回退恢复 Keil 下载，问题与本次大包版本有关；1024B 的具体根因
尚未定位。后续大包工作应先覆盖目标 RAM 的保存/块写/读回/恢复，再用单一
Keil 实例记录请求和响应，成功下载后才可提高默认容量。性能优化优先分析
小请求的网络往返和主机 IN 提交等待，不再仅凭探针只读基准扩大默认包长。
