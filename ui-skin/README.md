# ui-skin —— UI 换肤的素材与打包器（单一来源）

**目标**：换 UI 不重烧主控程序，只更换外置 SPI Flash 镜像。界面布局由镜像里的
`SKIN`（三屏，0x200）/`CNTL`（倒计时几何，0x100）数据块描述，固件读不到或校验不过就回退内置布局。
协议细节见 `notes/ui-skin-package-spec.md`、`notes/ui-skin-asset-spec.md`。

本目录是**唯一来源**：脚本、布局模板、三套皮肤素材、文档都在这里；`UI换肤交付包/`（仓库外）
只是发给美术的发行包，用 `tools/export_pack.py` 从这里导出，不要直接改它。

## 目录

| 路径 | 内容 |
|---|---|
| `scripts/` | 打包器：`pack_style.py`（入口）+ 各步骤脚本 + `ui_layout.py`/`countdown_layout.py`（布局编译） |
| `layouts/` | 布局模板：`ui_layout.guofeng.json`、`ui_layout.circus.json`、`countdown_layout.example.json` |
| `skins/<name>/` | 一套皮肤的素材 + `_清单.txt`（+ 数据驱动皮肤的 `ui_layout.json`） |
| `assets/px_digits/` | 08 号数字模板（打包时拷进产物的 `digits/`） |
| `assets/fonts/` | 打包用字体，**不入库**，见该目录 README |
| `docs/` | 交付规范 / 使用说明 / 回传清单（对外文档） |
| `tools/` | `pack_skin.bat`（拖拽打包）、`export_pack.py`（导出交付包）、`check_font_chars.py`（缺字自检） |
| `out/` | 打包产物，**不入库**（每次一个时间戳目录） |

## 三套皮肤

| 皮肤 | 布局 | 说明 | 验收镜像 sha256（前 16 位） |
|---|---|---|---|
| `skins/sample08` | 无 JSON（走内置 08 逻辑） | 08 号像素游戏机，基线回归用 | `8e83cd7c856c9261` |
| `skins/guofeng` | `ui_layout.json`（4bpp 字库 + 183 精灵） | 国风版，当前出货 | `5e90ab4d6adc3d39` |
| `skins/circus` | `ui_layout.json`（1bpp 字库 + 分段进度条） | 马戏团，验证换肤链路；有自带脚本 | 见下「可复现性」 |

```powershell
# 08 基线（哈希必须与上表一致，改了打包脚本先跑这个）
python ui-skin\scripts\pack_style.py ui-skin\skins\sample08

# 国风
python ui-skin\scripts\pack_style.py ui-skin\skins\guofeng

# 马戏团（自带 build_px_flash/build_px_font/circus_skin，用它的入口）
python ui-skin\skins\circus\scripts\pack_circus.py ui-skin ui-skin\skins\circus ui-skin\out
```

产物在 `ui-skin/out/<皮肤名>_<时间戳>/`：`flash_image_px.bin`（2 MB）+ 中间文件 + 素材快照 + `build_info.txt`。
烧录用**整个镜像从外置 Flash 的 0x000000 写满 2 MB**；开机串口出现 `[UI] SKIN flash (3 pages)` 即布局块生效。

## 加一套新皮肤

1. 在 `skins/` 下建目录，放三张干净背景、全显稿、精灵 PNG、机器人 GIF，写 `_清单.txt`（参考 `skins/guofeng/_清单.txt`）。
2. 复制 `layouts/ui_layout.guofeng.json` 或 `.circus.json` 成该目录的 `ui_layout.json`，按新素材改坐标/字号/精灵编号/进度条。
   打包器会校验边界、字号档、精灵号与文案宽度，越界直接报错。
3. 跑 `pack_style.py`，用固件仿真或实机看三屏；确认后把该目录与模板一起提交。

## 可复现性（重要）

- **同一台机器、同一套依赖**：重复打包逐字节一致（`sample08`/`guofeng` 已验证）。
- **换 Pillow/FreeType 版本**：字模与矢量绘制的抗锯齿会差 1 档，镜像不是逐字节一致（实测 44 个数字字形、
  冒号精灵各有几个像素不同）。**所以上表的哈希只在同一环境下可比**；跨环境请以三屏截图为准。
  记录 `build_info.txt` 里的 `px_font.bin` / `px_spr.bin` 哈希可以判断是"环境变了"还是"素材变了"。
- 字体文件换了（哈希不同）字模必变，见 `assets/fonts/README.md`。

## 不入库

`out/`（产物）、`assets/fonts/*.otf|*.ttf`（第三方字体）、`__pycache__/`。
仓库外还有：`UI产物/`（历史产物，~240 MB）、`字体合集/`（原始字体库）、`datasheets/ui设计/`（旧 08 流程脚本）。
