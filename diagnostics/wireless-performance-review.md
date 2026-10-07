# 无线下载性能检查（2026-10-06）

第一阶段只检查源码、构建配置及运行中探针的信息查询；没有改动或重刷固件，
没有连接、暂停目标或改写目标内存。测量脚本为 wireless_latency.py，
原始结果为 results/wireless-latency.json。

用户确认：当前使用 Keil 下载，程序几十 KB，总耗时约 15 秒，工程已打开。
这是用户观察的基线，镜像精确大小及擦除/编程/校验分项尚未测量。
因此当前已经不是 Keil 扫描不到的问题，应保持已工作的 WinUSB 设备形态。

## 包长

- 运行中 DAP_Info(FF) 公布 512 字节，DAP_Info(FE) 公布 8 包窗口。
- 高速 USB bulk 端点最大包长为 512 字节。USB/IP 每个 URB 的头为 48 字节；
  512 字节 OUT 请求在 TCP 字节流中占 560 字节（不计 TCP/IP/Wi-Fi 开销）。
- 并非每个下载命令都满 512 字节。实际包长分布及是否使用 8 包流水线需
  抓实际下载会话；本次信息查询不能代替这个测量。
- 32 位 DAP_TransferBlock 写请求有 5 字节头：最多 126 字即 504 字节目标数据；
  读响应有 4 字节头：最多 127 字即 508 字节目标数据。

## 只查询探针的实测

| 测试 | 次数 | 往返中位数 | P95 |
|---|---:|---:|---:|
| 2 字节 OUT，DAP_Info | 50 | 7.116 ms | 38.201 ms |
| 填充至 512 字节 OUT，同一 DAP_Info | 50 | 6.456 ms | 32.347 ms |
| 8 次 OUT 后收 8 个响应 | 20 轮 | 每轮 54.478 ms | 每轮 210.508 ms |

窗口测试平均为 117.5 命令/秒。脚本逐个等待 OUT 完成后再发下一个，
并非同时挂起 8 个异步 OUT。以上均不是 Flash 下载吞吐，也不包含 SWD
执行时间；但可以确认仅探针通信本身已有数毫秒延迟和明显长尾。

## Wi-Fi 和核分工

- wifi_ap_start 已在启动后调用 esp_wifi_set_ps(WIFI_PS_NONE)。
  当前为纯 SoftAP，CONFIG_PM_ENABLE 未启用，main 中无主动睡眠调用。
  没有发现当前固件自动降频/休眠路径；CPU 配置固定为 320 MHz。
- 非预期省电设置错误目前只记录 warning，尚无运行时查询核验。
- PHY 配置发射功率上限为 20 dBm，未启用降低发射功率选项。
  未显式设置/读取运行时 max_tx_power；实际发射功率还受速率、PHY 标定
  和国家区域配置影响。关闭省电不意味着始终发射，也不意味着每帧都为 20 dBm。
- ESP 关闭省电不能强制 Windows 无线网卡关闭它自己的省电。
- Wi-Fi 驱动任务：核 0；USB/IP、TinyUSB、CDC：核 0；DAP/SWD：核 1，优先级 22。
- lwIP TCP/IP 线程优先级 18，目前 NO_AFFINITY，可能在两核运行。
  因此无线底层和 SWD 已分核，但整条网络协议栈还没有严格限制在核 0。

## 优先优化顺序

1. 给实际下载会话增加低频汇总统计：请求长短分布、Transfer/TransferBlock
   比例、最大未完成数、队列等待时间、SWD 执行时间、响应就绪至发送耗时、
   WAIT 重试数及下载总耗时。避免逐包日志影响结果。
2. 移除 USB/IP 服务端的 10 ms select 超时轮询。核 1 完成 DAP 后应通过
   可唤醒网络等待的事件或受控发送任务通知核 0。不能仅以 task notification
   替换 select，因为网络到达和取消请求也必须及时处理。当前阻塞 recv_all
   等待半个报文时也可能推迟已就绪响应。
3. 每个 IN 响应的 48 字节头与数据目前分别 send，且启用了 TCP_NODELAY；
   可合并一次提交，减少小 TCP 段/调用（两次 send 不保证一定对应两个网络包）。
   OUT 每次 malloc/free 可换成会话复用缓冲。
4. 将 lwIP 固定核 0，并加启动日志读取省电模式、功率上限和任务当前核。
   当前已关闭 ESP 省电；提高发射功率不是已证实的主要提速点。
5. 确认下载器充分使用 TransferBlock 和已公布的 8 包窗口。先保持 512/8，
   根据占用率再决定是否提高 DAP 队列深度；USB HS bulk 单包上限仍为 512。
6. 单独测 SWCLK 实际频率和 SWD 吞吐。虽然 CONFIG_DAP_SPI_ACCEL=y，
   dap_swd_transfer 当前为 GPIO bit-bang；默认请求时钟 2 MHz 可被主机覆盖，
   周期延迟依赖估算开销，不能当作测得频率。网络延迟优化后，再考虑专用
   SWD 快路径/硬件移位，保持 turnaround、ACK、parity 和 WAIT 处理正确。

## 同时发现的正确性问题

无线描述符已为 reference 的单 WinUSB 三端点（01、81、82）。但 EP2 IN
仍进入 DAP 响应队列，并残留 64 字节 HID 填充逻辑；它应为独立 SWO 端点。
USB/IP ep 字段也不该接受带方向位的 0x81。当前未宣称 SWO 能力，若下载器
只访问 EP1 不一定触发，但探测第三端点可能抢走 DAP 响应。应作为独立修复，
不再改变 VID/PID 或接口拓扑。

物理 USB pump 和无线 USB/IP 共用没有来源标记的 DAP message buffers。
若同时使用两个传输，会有竞争/响应串路风险；需要会话独占或明确响应归属。
这不是本次单无线会话中已证实的慢速原因。

验收建议：相同目标、相同镜像、相同下载器和 SWD 时钟，分别记录擦除/编程/
校验时间，至少比较优化前后多轮中位数；保留 WinUSB 枚举、排队、取消重试、
目标数据读回及 EP82 不干扰 EP81 的回归检查。

## 第二阶段：已实现的优化

- DAP 响应写入 eventfd，直接唤醒 USB/IP 的 socket/eventfd 联合 select。
  接收部分 TCP 头或部分 OUT payload 时也会处理已就绪的 IN 响应。
- RET_SUBMIT 头和数据合并提交；每会话复用 RX/TX 缓冲，取消每个 OUT 的分配。
- lwIP TCP/IP 与 Wi-Fi 固定核 0；DAP/SWD 仍固定核 1，CPU 320 MHz。
- FreeRTOS 请求队列带来源及无线会话号，物理/无线响应分队列；
  无线导入期间暂停新的物理请求，重连丢弃旧无线会话的请求与响应。
- EP82 与 DAP 响应隔离；修复 other-speed 中第三端点的 64B 包长。
- 保持 WinUSB VID/PID/bcdDevice、单接口三端点、512B/8 包配置。
- 启动读回 Wi-Fi PS 模式和 TX 上限；保留 PHY 配置的 20 dBm 上限，
  不开省电和自动降频。上限不等于每一帧都按 20 dBm 发射。
- 每约 5 秒有流量时输出 perf 汇总；SWD 位时序未改变。

统计字段：out/bytes 为接受的 DAP 请求数/字节数；<=64/full512 为包长分布，
min/max 为实际 OUT 最小/最大字节数；transfer/block 为 05/06 命令数；
peak 为已接受尚未发出响应的最大包数；WAIT 是 SWD WAIT ACK 次数。
queue 是请求排队时间，execute 是 DAP 命令执行时间（包含必要的延时），
ready_to_send 是响应就绪到开始提交给 socket 的时间，也包含主机未提交 IN
导致的等待，不能单独解释成网络延迟。avg/max 单位均为微秒。
requested SWJ clock 是主机设置值，不是逻辑分析仪测得的 SWCLK。
每次日志后清零窗口统计，因此跨窗口的 out 和 done 不保证相等。

验证脚本：check_usbip_descriptors.py 检查链接后描述符；
usbip_transport_test.py 在 Windows 解除挂载时测试半包响应唤醒、EP82 隔离、
8 包连续在途、取消及断线重连；wireless_probe.py 测 Windows WinUSB API；
wireless_latency.py --output 路径保存独立测速结果。

## 第二阶段实机验收

构建、烧录、链接后描述符检查通过。Windows 已重新挂载到 USB/IP port 1。
启动日志确认 PS=0 (NONE)、TX ceiling=80 quarter-dBm (20 dBm)、Wi-Fi/USBIP
core=0、TCPIP affinity=0、DAP execution core=1、CPU 320 MHz。

首次尝试显式调用 esp_wifi_set_max_tx_power(80) 后出现连续 BROWN_OUT_RESET；
撤掉该调用、沿用 PHY 默认初始化后，多次启动正常，读回仍为 20 dBm。
保留此实测现象，不把它扩大解释为已经定位板级电源根因；最终固件不调用该 setter。

USB/IP 原始传输回归通过：部分头、部分 payload 时仍能发送已就绪 DAP 响应；
EP82 不抢 DAP 响应，非法端点返回 EPIPE；连续 20 轮 8 包；取消后重试；
5 次带未完成命令断线重连。该 Windows 环境下强制关闭有未读响应的连接时，
ESP 有时不及时收到关闭事件，新增 5 秒“响应无人读取/半包”超时回收，测试确认
回收后新会话不会收到旧响应；正常空闲无响应待取的连接不因此被回收。
另启用 TCP keepalive (idle 5s, interval 1s, probes 3) 及 2 秒发送超时。

Windows WinUSB API 回归通过：身份查询、20 轮队列、先 IN 后 OUT、取消重试；
目标读回 DPIDR=0x6BA02477、CPUID=0x411FC271。此次没有改写目标 Flash。
结果分别保存为 results/usbip-transport.json 和 results/wireless-winusb.json。

同一信息查询脚本的优化后结果：results/wireless-latency-after.json。

| 测试 | 优化前中位数 / P95 | 优化后中位数 / P95 |
|---|---:|---:|
| 2 字节 OUT | 7.116 / 38.201 ms | 7.197 / 25.865 ms |
| 512 字节 OUT | 6.456 / 32.347 ms | 6.790 / 26.447 ms |
| 8 次 OUT 后收 8 个响应 | 54.478 / 210.508 ms | 64.677 / 139.414 ms |

窗口平均命令率 117.5 -> 108.4 次/秒；本轮未证明吞吐或中位延迟改善。
P95 降低也只是单轮观察，Wi-Fi 环境/客户端调度存在波动，不能据此宣称下载提速。
该脚本等 OUT 完成后才提交 IN，不能单独衡量预提交 IN 场景下 eventfd 的收益。
用户已用原 Keil 工程完成下载，反馈“下载成功了，速度似乎差不多”。

## Keil 实际下载统计与后续方向

日志：results/performance-acceptance-uart.log，149526 ms 开始的会话。
四个统计窗口合计 2039 个 DAP 请求、156979 字节 OUT；这些是协议流量，
包括 Flash 算法传输/编程/校验控制等，不是应用镜像大小。
其中 1678 个请求 <=64 字节（约 82%），1624 个 Transfer、389 个 TransferBlock；
实际最大请求 509 字节，符合 5B 头 + 126 个 32 位数据（504B）。
窗口峰值 2～3，未用满公布的 8 包。Keil 请求 SWJ 时钟 10 MHz，未测实物波形。

各窗口平均 queue 61～188 us，execute 226～1013 us，ready_to_send 3192～4121 us。
共记录 2452 次 WAIT ACK；WAIT 不等于错误，可能是目标忙。此次各项统计只包含
探针内部时间，不能把它们简单相加当作下载总耗时，也不能直接分解擦除/编程/校验。

当前证据不支持优先扩大队列（峰值仅 3）或盲目再提高请求时钟（已请求 10 MHz）。
下一步优先抓同一下载会话的 OUT/IN 提交时刻，分清主机串行等待与 Wi-Fi 往返；
然后比较物理 USB 与无线的同镜像下载时间，量化网络部分的占比。
只有确认 SWD 执行成为主要耗时后，再做实际 SWCLK 测量和 bit-bang/硬件移位优化。
本轮停止串口抓取并释放 COM6，设备保持 Windows 无线挂载，供用户正常使用。
