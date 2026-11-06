# payload-dumper 使用说明

```bash
./build/release/payload-dumper [选项] payload.bin或ota.zip
```

## 选项

| 选项 | 说明 |
| --- | --- |
| `-c N` | 解包 worker 数，默认逻辑 CPU 数；另建 `max(1, floor(N / 2))` 个校验 worker |
| `-o DIR` | 输出目录，默认 `extracted_时间戳` |
| `-p LIST` | 指定分区，多个名称以逗号分隔 |
| `-l` | 列出分区，不解包 |
| `-old DIR` | 增量更新使用的旧版分区镜像目录，文件名为 `分区名.img` |
| `-q` | 静默模式 |
| `-m` | 机器可读进度，输出格式为 `分区名:百分比` |
| `-no-verify` | 跳过源数据及最终镜像校验，仍校验 operation blob |
| `-no-fec` | 跳过 FEC 重建；受影响分区也跳过最终镜像校验，生成的镜像不可刷写 |
| `-h` | 显示帮助 |

## 示例

```bash
# 12 个解包 worker + 6 个校验 worker
./build/release/payload-dumper -c 12 -o out payload.bin

# 只提取指定分区
./build/release/payload-dumper -p boot,system -o out payload.bin

# 列出分区
./build/release/payload-dumper -l payload.bin

# 直接解包 OTA ZIP
./build/release/payload-dumper -o out ota.zip

# 增量更新：先提取旧版完整 OTA，再应用增量 OTA
./build/release/payload-dumper -o base_images base_ota.zip
./build/release/payload-dumper -old base_images -o new_images delta.zip
```

## 图形界面（可选）

仓库内提供一个 GTK4 / libadwaita 前端，功能与上面的命令等价，适合交互式
选择分区和解包：

```bash
./ui/payload-dumper-gui
# 或
make gui
```

需要额外的 `python3-gi`、`gir1.2-gtk-4.0`、`gir1.2-adw-1` 依赖（Debian /
Ubuntu 包名）。界面会自动查找 `build/release/payload-dumper`，也可以通过
`PD_BIN` 或右上角设置指定其他路径。详细说明见
[ui/README.md](ui/README.md)。

## 完成用时

正常模式成功解包后显示总用时，例如：

```text
Extracted 67 partition(s) to out
Elapsed time: 22.754 s
```

用时包含打开输入、解包、校验及关闭输出，不代表数据已同步持久化到磁盘。
`-q`、`-m` 和仅列出分区时不显示用时摘要。
