# Phigros HarmonyOS Adapter

将 Android ARM64 Unity Player 运行于 HarmonyOS 的实验性兼容层。当前源码对应转译层 **1.0.6**，匹配 **Phigros Android 4.0.1.2**。

这是社区研究项目，与 Phigros、Pigeon Games、Unity、TapTap 或华为没有官方隶属关系。仓库发布的是兼容层源码，不包含原游戏、Unity 引擎、曲绘、音乐、谱面、存档或签名证书。

## 实现与状态

- 使用原 Android ARM64 引擎与 IL2CPP，补充 JNI、线程同步、资源读取、网络与系统接口。
- 通过 ArkUI XComponent、OHNativeWindow 与 EGL 输出画面；保留原生触摸时间戳。
- OpenSL ES/FMOD 音频桥、真实视频解码与 NativeImage 视频纹理。
- 系统输入框和原生提示框、原游戏退出请求、前后台暂停/恢复与窗口生命周期。
- TapTap 登录及实名相关交互桥接；云存档仍由原游戏逻辑和服务处理。请只在官方授权页面完成账户操作。
- 申请最高 120Hz，读取设备实际刷新率；这不代表每台设备或每个场景恒定 120fps。

已在一台 HarmonyOS 平板上测试。1.0.6 的 PV 后台返回及游玩自动暂停问题由测试者确认解决；没有覆盖全部设备、PV 组合或第九章具体密码页面。仍可能有加载卡顿和版本相关问题。**不是通用 Android 模拟器，也不能直接支持任意 APK 或任意游戏版本。**

## 构建

需要 Python 3.10+、DevEco Studio、HarmonyOS SDK 与其 Hvigor 插件。当前配置保留已使用的 SDK：`compileSdkVersion=26.0.0`，`compatible/targetSdkVersion=6.1.1(24)`，ARM64。请在 DevEco SDK Manager 中安装对应组件；更改 SDK 后应重新验证。

1. 克隆仓库并准备你自己持有的 **4.0.1.2 Android APK**。无需填写个人姓名、手机号或邮箱。

   ```powershell
   python tools/prepare_game.py "路径/Phigros_4.0.1.2.apk" --check
   python tools/prepare_game.py "路径/Phigros_4.0.1.2.apk"
   ```

   脚本检查引擎与元数据的尺寸和 CRC，从 APK 提取 4 个 ARM64 库及 763 个启动文件，并将 native-audio 库的 OpenSL ES 依赖改为本项目的音频桥。提取内容受到 `.gitignore` 排除，**不得提交进源码仓库**。不依赖原 APK 的文件名或压缩后总大小。应用内导入另有完整 assets 指纹检查。

2. 用 DevEco Studio 打开仓库目录，完成工程同步，构建 `entry` 的 Debug HAP。仅有 `entry` 模块，不需要团结引擎或 `tuanjieLib`。`liblog.so` 等兼容库由 CMake 从源码生成。

   Windows 命令行也可使用：

   ```powershell
   ./build.ps1 -DevEcoHome "你的 DevEco Studio 安装目录"
   ```

   若单独运行 Hvigor 时找不到 `@ohos/hvigor-ohos-plugin`，先在 IDE 中同步并构建，或将本地插件目录通过 `-HvigorModules` 指定。插件安装和 SDK 不包含在仓库里。

3. 默认工程没有签名配置。未签名产物位于 `entry/build/default/outputs/default/entry-default-unsigned.hap`。安装需用自己的开发者配置签名，并授权自己的测试设备；不要提交证书、私钥或密码。

4. 首次运行在应用中选择相同版本的原 APK 导入歌曲资源；之后无需重复选择。覆盖安装保留存档需要兼容的包名及签名，先备份自己的数据。新项目默认使用现有兼容包名 `com.rea.message.demo`，不要把未知签名的构建直接覆盖重要数据。

## 源码导航

| 位置 | 内容 |
| --- | --- |
| `entry/src/main/cpp/jni_emu.cpp` | JNI 对象/调用、原 Unity 启动与主循环 |
| `pthread_abi.cpp` | Android 线程 ABI 与窗口调用桥接 |
| `host_lifecycle.h`, `surface.cpp`, `native_touch.cpp` | 前后台、窗口生命周期、低延迟触摸 |
| `android_media.cpp`, `video_surface.cpp` | 音视频解码及视频纹理 |
| `android_sles_audio.cpp`, `fmod_audio_host.cpp` | 音频输出 |
| `player_platform.cpp` | 输入法、原生弹窗与退出 |
| `java_preferences.cpp` | 本地偏好原子写入与恢复 |
| `entry/src/main/ets/probe/` | Ability、APK 导入、账户交互和系统能力 |
| `tools/` | 本地构建输入准备及源码发布检查 |

本版本包含特定引擎地址、结构与 ABI 假设。升级 Android APK 需要重新检查 JNI 注册、IL2CPP 槽位、FMOD 地址、资源指纹及媒体行为，不能仅改版本号。

## 贡献与隐私

提交问题请提供系统版本、复现步骤、曲目和大致时间。日志分享前删除登录令牌、用户 ID、设备序列号、云存档地址和私人内容；**不要上传完整存档或登录凭证**。运行 `python tools/check_public_tree.py` 检查待发布文件。

原始开发与测试由 sxyzc233 组织，过程中使用了 DeepSeek 与 Codex 辅助。当前发布从已测试的 1.0.6 转译层整理而来；开源工程移除了历史团结模块、机器专用配置和原游戏图标，改为独立中性图标。

## 许可证

兼容层源码采用 [MIT](LICENSE)。第三方游戏、引擎、SDK 与素材不在该许可范围内，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
