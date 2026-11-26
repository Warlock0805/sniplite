# sniplite-pin 维护手册

## 项目和分支

| 名称 | 用途 |
| --- | --- |
| `origin` | 自己的 fork 和安装包发布渠道。[Warlock0805/sniplite](https://github.com/Warlock0805/sniplite) |
| `upstream` | 截图和贴图程序的上游源码。[MrTangLuyao/sniplite](https://github.com/MrTangLuyao/sniplite) |
| `main` | 跟随上游 `main`，供同步比较。 |
| `pin-enhancements` | 贴图增强和 fork 更新渠道所在分支。日常开发和构建用此分支。 |

贴图交互参考 [tylearymf/SETUNA2](https://github.com/tylearymf/SETUNA2)。两项目的署名见 [LICENSE](../LICENSE)，用户操作见 [简体中文 README](README.zh-CN.md)。

## 构建 Windows x64 安装包

在仓库根目录打开 PowerShell。可使用已解压的 w64devkit 2.11.0。仅在当前终端调整 PATH。

```powershell
$snipBin = Join-Path (Get-Location) 'build\toolchains\w64devkit-2.11.0\w64devkit\bin'
$env:PATH = "$snipBin;$env:PATH"
gcc --version
windres --version
cmd.exe /d /c build.bat
if ($LASTEXITCODE -ne 0) { throw 'sniplite 构建失败' }
& .\build\sniplite.com version
Get-FileHash -Algorithm SHA256 .\build\sniplite_win_setup_amd64.exe
```

全新克隆可安装 [w64devkit](https://github.com/skeeto/w64devkit) 或使用已有的 MinGW-w64，并把 `$snipBin` 指向对应 `bin`。运行 [build.bat](../build.bat) 会生成 `build/sniplite.exe`、`build/sniplite.com` 和 `build/sniplite_win_setup_amd64.exe`。安装包内嵌前两个文件。生成带 OCR 的便携包时，先按 [pack.bat](../pack.bat) 的清单准备 `build/ocr` 的模型和运行库，再运行该脚本。

## 发布和升级

发布安装版时，在 [GitHub Releases](https://github.com/Warlock0805/sniplite/releases) 创建 `vX.Y.Z` 正式版，把标签指向实际用于构建安装包的提交，并上传 `sniplite_win_setup_amd64.exe`。应用按这个文件名下载更新，ZIP 可作附加下载。发布后打开 [Latest](https://github.com/Warlock0805/sniplite/releases/latest)，核对页面标签和附件。

后续升版时同时核对 [src/common.h](../src/common.h)、[res/sniplite.rc](../res/sniplite.rc) 和 [res/setup.rc](../res/setup.rc) 中的版本号。软件比较标签的三段数字版本，只有高于当前版本才提示更新。

- 软件更新仓库由 [src/update.h](../src/update.h) 的 `UPDATE_REPO` 指定。当前为 `Warlock0805/sniplite`。
- OCR 模块和运行库由 [src/ocr_get.c](../src/ocr_get.c) 指向上游 `ocr-2`，使用代码里固定的校验值。
- 升级验证使用隔离目录，检查程序能启动、`sniplite.ini` 原样保留、OCR 文件和开机启动项仍在。模拟程序文件被占用，检查旧版能回滚并重启。用户配置 `sniplite.ini` 和未关闭贴图缓存 `sniplite.pins` 都在可执行文件旁。

### 已发布基线

截至 2026-11-26，[v1.5.1](https://github.com/Warlock0805/sniplite/releases/tag/v1.5.1) 为 fork 的最新正式版，指向提交 `19e3003645941921c1d0ec5c9e6ab3fccbd86b1e`。GitHub 附件为 `sniplite_win_setup_amd64.exe`，大小 441856 字节。以下 SHA-256 与本地安装包一致。

```text
4a65e3523257467777ffbaeec0f3df68c51563d0c7df104be2c3068d29debc7e
```

## 同步上游

从工作区干净的 Git 副本开始。如果 `git remote -v` 里只有 `origin`，先补上上游地址。

```powershell
git remote add upstream https://github.com/MrTangLuyao/sniplite.git
```

```powershell
git fetch upstream
git switch main
git merge --ff-only upstream/main
git push origin main
git switch pin-enhancements
git merge main
```

如果任一步失败，先处理报错再执行后续命令。合并时重点复核贴图交互、配置读写、OCR 下载和 fork 的更新地址。解决冲突后重新构建，检查双击收起、误关恢复、重启恢复、贴图快捷键和安装升级，然后将 `pin-enhancements` 推到 `origin`。GitHub 源码同步和软件内的版本更新各自独立。
