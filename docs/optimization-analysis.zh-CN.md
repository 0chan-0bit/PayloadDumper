# 真实 payload 性能分析（2026-11-07）

## 范围与环境

阅读 README、C++ 解压/调度/IO/verity/FEC 代码及本地 Go 实现后分析。
本轮保留提取器实现不变，新增真实样本基准脚本，以免把未经验证的优化计入比较。
Go 参考版本为本地干净 checkout：c3a50ba8d784764c06143c1f82971cd81a77f9d6。
C++：GCC 14.2.0，现有 make release（O3/native/LTO）；Go：官方 Go 1.27.0，默认 go build，CGO 开启。
系统 liblzma 5.8.1，OpenSSL 3.5.7；Ryzen 5 7600，6 核/12 线程，约 30 GiB 内存，NVMe。
Go 的 XZ 同样调用 C 库，不能把收益全部归因于 C++ 对 Go 的语言差异。

样本 payload.bin：9,550,378,792 字节；67 个分区；输出 12,958,371,840 字节。
6,221 个操作：REPLACE_XZ 5,583、REPLACE 350、ZERO 193、REPLACE_BZ 95。
所有操作各有一个目标 extent。本样本无需旧镜像，也没有待重建 hash tree/FEC extent。
最大分区 my_stock 4.739 GB/2,260 个操作，odm 3.106 GB/1,482 个操作。

## 测量方法

使用 tests/bench_real.py，默认校验全部开启，两个程序均 -q，明确指定 -c 1/6/12。
最初计划各 3 次；用户追加 RAM 缓存要求后，中止自然缓存后续轮次，保留已完成结果。
显式 RAM 预热测试对 6/12 worker 各执行 3 次，第二轮反转程序执行次序；串行运行，避免两个提取器争用资源。
每次创建全新输出目录，计时包括启动、解压、写入和程序内校验；不包括构建、外部散列与删除。
自然缓存第一轮单线程及 RAM 预热第一轮 6 worker 输出再以独立 Python SHA-256 核对全部 67 个镜像；
原始散列与逐次数据分别保留在 benchmarks/real 和 benchmarks/ram。
这是普通桌面环境的缓存混合测试：不清系统页缓存，不控制其他桌面负载，也不声称严格冷盘。
预热曾运行一次 C++ 12 worker（24.811 秒），不计入正式统计。
两版均不 fsync，因此时间表示进程完成和写入页缓存，并非断电持久化耗时。
RSS 取 Linux wait4 的 ru_maxrss；mmap 的驻留文件页计入 C++ RSS，不能直接当成堆内存比较。

复现：

```sh
make -j12
# 在 payload-dumper-go 中用 Go 1.27 构建：go build -o ../build/payload-dumper-go .
python3 tests/bench_real.py --go build/payload-dumper-go --runs 3 --workers 6,12 --warm-input --work benchmarks/ram-new
```

## 现有优化与进一步空间

### 1. 已有操作级并行，优先保留

src/payload.cpp 的 Phase 1b 把每个操作放入全局线程池；同一大分区可使用所有 worker。
Go 的 payload/extract.go 仅在分区间并行，同一分区 runOp 顺序执行。
因此 my_stock 会给 Go 留下很长的串行尾部，增加 -c 超过大分区数量的收益有限。
C++ 已直接解压到输出 mmap，并对 ZSTD 使用 thread_local context；不能把这些作为尚未实现的优化。

### 2. 优先补齐并行写入的前置检查

src/extents.cpp:validate_extents 只检查可寻址范围，不检查目标是否超过 new_size，
也没有跨操作 extent 重叠检查；run_replace 的 DirectSink 和 run_zero 直接做指针写入，
绕过 OutFile::write_at 的文件边界检查。畸形输入可能越界，同一目标的重叠操作可能数据竞争。
建议在提交任务前统一检查 extent 的边界、求和溢出和互斥性；对有合法顺序依赖的操作保留顺序执行路径。
这是进一步调整并行调度前的正确性条件，不应以最后的散列校验代替。

### 3. 分区完成后尽早校验，降低全局 barrier 的尾部

目前全部操作 pool.wait 后，再串行遍历分区生成 verity，最后按分区并行 SHA-256。
最后的大分区 SHA-256 内部仍是单线程，可能使多数核心空闲。
建议让已完成且无需 verity 的分区提前进入校验任务，与其他分区解压重叠；
校验大分区优先以降低收尾时间，同时注意读写带宽竞争。
这只是代码层面的候选优化；本轮没有做阶段计时/CPU profile，不能声称 SHA 是最大热点或承诺其收益。
有 verity 的分区需要依赖图或独立协调器，不能从 pool worker 中递归调用当前 parallel_for。
标准 SHA-256 不能将任意块分别散列后拼接成同一个最终摘要；不能直接替换成树形哈希。

### 4. XZ 初始化/分配与调度

src/decompress.cpp 每个 XZ 操作都 lzma_stream_decoder/lzma_end。
README 所称 thread-local 解码上下文实际上仅 ZSTD 明确实现；XZ/BZ2/Brotli 没有同等复用。
5,583 次 XZ 初始化值得通过分配次数和 CPU profile 确认；可实验 thread-local allocator/解码器重初始化复用。
约 2 MiB 的操作已足够供外层并行，不宜再给每个 XZ 操作开多线程而造成 N×M 线程超额。
现有逐操作 FIFO 会按分区提交；可比较大任务优先、有限窗口及按数据偏移排序，平衡长尾与顺序预读。
任务数只有约 6 千，队列锁未必是热点，应先测量再做无锁队列。

### 5. 修正 parallel_for 的线程数语义与忙等

src/pool.h:parallel_for 在 N 个后台 worker 之外，让调用线程也参与，最多 N+1 个执行者；
因此 -c 1 在最终校验等阶段并非严格单核限制。正式比较的是 CLI 配置值，而非 CPU affinity。
尾部用 yield 自旋等待所有 helper 返回，可能浪费核心；可实验 atomic::wait/notify 或条件变量。
小任务应顺序执行，hash tree 的 4 KiB 粒度可批量分配以减少原子计数器竞争。
该样本没有 hash tree/FEC，相关收益需用独立 delta 样本验证。

### 6. IO 与内存

mmap 仍有缺页、页表、内存复制及后台回写成本，README 的“热路径无 read/write syscall”不等于零 IO 成本。
可以实验 MADV_SEQUENTIAL/有界预读，但需要比较冷读、热读和磁盘压力下的结果。
ZERO 目前 memset 所有区域；对新建且从未写入的零区域可在确认 extent 不重叠后保留零页/洞，
但增量来源和预分配策略要单独处理，不能无条件跳过清零。
copy_fallthrough 以 gap 为单位并行，一个巨大 gap 仍只有一个任务；可按固定大小拆分。
BSDIFF 保留完整 patched buffer；连续目标可研究直接写入以节省额外复制，但要保留 patch 完整性检查。

### 7. 构建与基准维护

Makefile 文档中的 PORTABLE=1 未被读取，实际控制变量是 NATIVE=0；应对齐文档或添加变量支持。
更换编译选项后 make 不会因 flags 改变自动重编，独立 BUILD 目录更可靠。
PGO 应使用代表性的真实 XZ 样本，不能只靠随机数据/preset=0 的合成样本。
建议依次实验“提前分区校验”“XZ 分配复用”“6 对 12 worker”，每次保留同样的完整性验证。
本轮没有修改这些实现，也没有把预期收益写成已验证收益。

## RAM 缓存补测

用户要求尽量缓存输入后，基准脚本新增 --warm-input：计时前使用 16 MiB 可复用缓冲区顺序读完整个输入，
然后通过 Linux mincore 统计驻留页比例；解包后再次统计。预热耗时单列，不混入提取器耗时。
不复制到 tmpfs，避免同一输入在页缓存和 tmpfs 各占一份内存；不强制锁页，以便内核为输出回写分配空间。
驻留比例是采样时刻的状态，不能保证计时期间完全没有输入页回收。

## 实测结果

RAM 预热测试：每组 3 次，计时前输入驻留率均为 100%；预热约 0.94–2.54 秒，不计入提取耗时。

| worker | 实现 | 中位数（秒） | 最小–最大（秒） | 输出吞吐（MB/s） |
| --- | --- | --- | --- | --- |
| 6 | cpp | 35.22 | 34.46–35.33 | 367.9 |
| 6 | go | 63.27 | 62.68–73.17 | 204.8 |
| 12 | cpp | 26.88 | 24.16–28.22 | 482.0 |
| 12 | go | 60.47 | 60.17–72.78 | 214.3 |

同 worker 中位数比：6 worker C++ 快 1.80 倍，12 worker C++ 快 2.25 倍。
C++ 12 worker 相对 6 worker 提速 1.31 倍，耗时下降 23.7%。

自然缓存已完成轮次（每组一次，不与 RAM 组三次中位数混合）：
| worker | C++（秒） | Go（秒） |
| --- | --- | --- |
| 1 | 161.36 | 166.89 |
| 6 | 31.17 | 61.21 |
| 12 | 21.83 | 中断，未计入 |

正确性：自然缓存单线程与 RAM 预热 6 worker 第一轮各自对比全部 67 个镜像，SHA-256 完全一致；两种条件下的散列也一致。全部正式已完成提取均默认开启内部校验，退出码 0。
输入在提取结束时驻留率范围为 62.7%–100%，输出分配期间仍可能回收输入。

结论：本机本样本优先使用 C++ -c 12。Go 的分区内串行使增加 worker 的收益有限。
预热读入 RAM 可排除大部分起始读盘因素，但本次没有观察到相对自然缓存单次结果的明确加速；不能把不同轮次差异解释为 RAM 缓存的因果收益。真实 XZ 数据的结果也不能直接套用 README 的随机合成数据吞吐。

候选优化未经实现或 profile 证实；不将其预期收益混入以上数字。
逐次命令/CPU/RSS/驻留率见 benchmarks/ram/results.jsonl，聚合数据见 summary.json，环境和二进制散列见 benchmarks/environment.json。
