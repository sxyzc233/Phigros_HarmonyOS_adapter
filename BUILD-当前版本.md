# PhigrosXS 4.1.0.1 构建

- 包名 com.pgrhmos.transcode.demo，版本名 4.1.0.1，内部版本号 162。
- 基于 PhigrosXS v13-dlfix。使用 HarmonyOS 26.0.0 SDK 编译，目标和最低兼容版本均为 6.1.1(24)。
- 先使用 tools/prepare_game.py 和 4.1.0.1 原 APK 准备四个 ARM64 库，再运行 build.ps1。HAP 位于 entry/build/default/outputs/default/entry-default-unsigned.hap。
- 导入页接受本地 APK 或 HTTPS 直链。直链下载必须匹配 ApkProfile.ets 中的大小和 SHA-256。
