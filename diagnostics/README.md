# CMSIS-DAP 1.1.0 修复与实机验收

日期：2026-10-05。工作目录：`C:\Users\JZM\softAP`。

## 结果

新版固件已通过 COM6 烧录到 ESP32-S31 探针。通过本机 Keil 的
CMSIS-DAP 驱动和 STM32F10x_128.FLM 完成目标程序擦除、下载、校验：

```
Load "..\\OBJ\\Template.axf"
Erase Done.Programming Done.Verify OK.Flash Load finished at 18:17:05
```

见 `results/keil-download.log`。本次下载没有出现 "Not a genuine ST Device"。
下载后的 4664 字节 ELF 加载段经独立 OpenOCD 读回比较完全一致。
下载前后整个 128 KB Flash 也完全一致，原程序已保留，最后执行了复位运行。

三份 Flash 文件的 SHA-256 都是：

`F18F90EE07BD659BDB71A8DAA912D25C8D11CC183D0B694D83846BB7D45E49CC`

- `results/target-original-128k.bin`：下载前 bulk 读取。
- `results/target-original-hid-128k.bin`：下载前 HID 独立读取。
- `results/target-after-keil-128k.bin`：Keil 下载后的独立读取。

原桌面工程、驱动、注册表和已安装的 Keil 设备包均未修改。
`keil-test/` 是独立验收工程副本，选择 CMSIS-DAP；其程序来自原工程的
`Template.axf`，该加载段在测试前就与目标 Flash 相同。

## 固件修改

- 实现 DAP_TransferBlock 的读、写、16 位计数、AP posted-read、WAIT 重试和末次写检查。
- 修正普通传输的匹配失败标志，预检请求长度和响应容量。
- 区分 JTAG 与 SWD 的 posted-read 规则；JTAG 本次未连接硬件验收。
- USB 回调立即复制完整请求，保留来源接口，提供与公布的 8 包窗口匹配的队列。
- bulk 前一响应完成后才提交下一响应，防止短响应在 FIFO 中合并。
- 等待 HID 就绪时保留响应，USB 会话变化时丢弃旧会话响应。
- 实现 SWD_Sequence 输入/输出及 USB 接收侧的 TransferAbort。
- DAP_Delay 使用微秒；SWJ_Pins 解析完整 7 字节命令并实现等待。
- DAP_Info 字符串长度包含 NUL，固件版本为 1.1.0。
- 不支持的命令按标准返回 ID_DAP_Invalid；未宣称支持原子命令、SWO 等扩展。

## 实测覆盖

见 `results/protocol-hid.json` 和 `results/protocol-bulk.json`：

- HID 和 WinUSB bulk 两接口；设备固件版本 1.1.0。
- 连续提交 8 个请求再接收，20 轮逐个比较响应内容。
- 普通和批量命令读取 DP、AP 及目标内存。
- 1 KB SRAM 数据模式写入/读回，普通写与批量读、批量写与普通读交叉验证。
- SRAM 测试后恢复并校验原内容。
- Keil CheckID 等价检查：ROM base=0xE00FF000，PIDR1=0x04，PIDR2=0x0A，厂商值=0x20。
- Keil 实际擦除、编程、校验以及独立 Flash 读回。

旧固件整段 Flash 读取曾出现 `Sent 0x12 received 0x6` 响应错位。
修复后批量读取成功；这证明旧固件数据通路并不完整。
未抓取旧 Keil 失败会话，因此不能把旧弹窗唯一归因于某一处代码。

## 复验

关闭占用探针的调试会话，依次运行（不得同时连接两个客户端）：

```powershell
python diagnostics\dap_probe.py --backend hid
python diagnostics\dap_probe.py --backend bulk
```

上述脚本使用 Windows 自带 HID/WinUSB API，无需安装 Python 包、驱动或修改配置。
它会暂时暂停目标、备份并测试 1 KB SRAM，然后恢复内容与原运行/暂停状态。
不擦除 Flash。

`target.cfg` 仅用于 OpenOCD 读内存/调试。此目标 CPUID 为 Cortex-M4，
DBGMCU ID 为 0x20036410；OpenOCD stm32f1x 自动探测会错误选择
0x1FFFF7CC 作为容量地址，得到不可信的 Flash 容量。不能用该自动探测结果擦除。
这不影响已验收的 Keil STM32F10x_128.FLM 下载路径。

## 构建

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py build flash -p COM6
```

产物：`build/softAP.bin`。构建成功，应用大小 0xF07A0，1 MB 应用分区剩余约 6%。
修改前的主程序源码和探针二进制保存在 `before-fix/`。
本次实机验收范围是当前板子的 USB CMSIS-DAP SWD 下载；不代表完成了
JTAG、无线传输或所有 CMSIS-DAP 可选扩展的硬件验收。
