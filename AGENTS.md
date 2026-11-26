# sniplite-pin 开发说明

- 这是基于 sniplite 的 Windows 截图与贴图工具。贴图交互参考 SETUNA2，使用仓库原有的 C、Win32 和 x86-64 汇编实现。
- 日常开发使用 `pin-enhancements`。`main` 跟踪上游，`origin` 指向 `Warlock0805/sniplite`，`upstream` 指向 `MrTangLuyao/sniplite`。
- 从仓库根目录运行 `build.bat`。先让 MinGW-w64 的 `gcc` 和 `windres` 在 PATH 可用。安装包输出为 `build/sniplite_win_setup_amd64.exe`。
- 贴图窗口和会话恢复见 `src/pin.c`，偏好和快捷键见 `src/config.c`、`src/settings.c`，界面翻译见 `src/lang_text.h`，对话框资源见 `res/sniplite.rc`。调整交互时同步校对快捷键、界面和用户说明。
- 已安装程序的配置和贴图缓存与可执行文件相邻。升级验证使用隔离的安装目录和测试设置，覆盖配置保留、程序启动和失败回滚。
- 用户用法见 [简体中文 README](docs/README.zh-CN.md)。构建、更新渠道和上游同步见 [维护手册](docs/MAINTENANCE.zh-CN.md)。
