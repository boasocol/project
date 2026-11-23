# 打包用字体（不入库，本机自备）

打包脚本按 `_清单.txt` 里的 `font_face` 取字体；没写 `font_face` 的风格（08 号基线、马戏团）
用这里的 `SourceHanSansSC-*.otf`。字体文件体积大（合计 ~93 MB），**不纳入 git**，换机器按下表自备。

| 文件 | 用途 | 大小 | sha256（前 16 位） | 来源 |
|---|---|---|---|---|
| `SourceHanSansSC-Medium.otf` | 08 号基线小字号（12/13/14px） | 15.8 MB | `1df61d31687d04fd` | 思源黑体 SC，Adobe，OFL 1.1 |
| `SourceHanSansSC-Bold.otf` | 08 号基线大字号（26px）/ 马戏团数字 | 16.2 MB | `df2b90f5bcc6d01d` | 同上 |
| `SourceHanSerifCN-Bold.otf` | 国风大数字精灵（宋体粗体） | 11.5 MB | `4ee555ae58b3d22f` | 思源宋体 CN，Adobe，OFL 1.1 |
| `LXGWWenKai-Medium.ttf` | 国风正文（12/13/14/16px，4bpp） | 24.6 MB | `f4b8a34d1f838fe4` | 霞鹜文楷 Medium，OFL 1.1 |

- 下载：思源系列 <https://github.com/adobe-fonts/source-han-sans> / <https://github.com/adobe-fonts/source-han-serif>；
  霞鹜文楷 <https://github.com/lxgw/LxgwWenKai>。
- 也可以不放这里，改用环境变量 `UI_FONT_DIR` 指向任意目录（优先级最高）。
- 校验：`Get-FileHash <file> -Algorithm SHA256`，前 16 位应一致；不一致时打包出来的字模会变（见 README 的「可复现性」一节）。
