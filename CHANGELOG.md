# Changelog

## 4.1.0.1

- 使用 PhigrosXS v13-dlfix 的多指触控和线程同步改动，更新 Android 4.1.0.1 引擎输入及资源指纹。
- 应用包名为 com.pgrhmos.transcode.demo，版本号与原 APK 一致。
- 首次启动支持 APK HTTPS 直链下载，校验通过后导入。

## 4.1.0（多指输入同步修复，本地分支）

- 触摸上报改为整帧全量同步：XComponent 读 `numPoints`/`touchPoints[]`，ArkTS 回退读 `TouchEvent.touches`，交给 `ReaQueueTouchSnapshot` 与引擎持有的触点集合做差集，按「先释放、后按下、最后一帧一次 MOVE」的顺序补发。
- 修复同帧内第二、三根手指的 Down 丢失后，其后续 Move/Up 被 `SubmitTouch` 的 `existed` 判断全部丢弃而导致的多指断触。
- 修复同帧内丢失的 Up 留下幽灵触点、使最后一根手指抬起时不发 `ACTION_UP`（音符一直按住 / 抬手不判定）的问题。
- `SubmitTouch` 对同一 id 的重复 Down 只更新位置，不再重复注入，避免平台复用触点编号时整个指针被重启。
- 独占触屏装上时先发 cancel，清掉窗口路径残留的触点。
- 新增 `TOUCH_SNAPSHOT` 日志（前 8 条），输出每帧 `points/added/removed/down`。
- 帧内历史点回放：`DispatchTouch` 先取 `OH_NativeXComponent_GetHistoricalPoints`，把本帧内 `DOWN/UP` 按时间序补发（陈旧点超过 50 ms 丢弃），再做帧末全量对账；新增 `TOUCH_FRAME_WINDOW` 统计行（`frames/histDown/histUp/popped/points`）。
- 真机结论：这台设备上历史点为空（`histDown/histUp` 全程 0），窗口路径每帧只给一个状态快照，帧内原始点位拿不到；而独占触屏（`OH_Input_AddInputEventInterceptor`）是逐事件、不过帧合并，实测 `next_render_p50` 21.1 → 15.8 ms。
- 因此「独占触屏」默认开启（`inputInterceptWanted` 初值 true），首次进入游戏画面时一次性申请 `INTERCEPT_INPUT_EVENT` 权限；开关仍保留在适配选项里，可随时还给窗口路径。
- 修掉 `applyInputIntercept` 里原本写空的失败提示语句，启动失败时能正常写入 `inputInterceptNote`。
- 触摸注入改为专用线程 FIFO 队列：拦截器/XComponent 回调只做时间戳归一化和入队，注入（一次 JNI 进引擎，实测最坏 55 ms）不再占着系统回调线程；队列 1024 条，溢出丢弃并计数。新增 `TOUCH_INJECT_QUEUE applied/dropped/maxWaitMs` 与 `INJECT_STALL` 日志。
- 刷新率请求默认从 120 Hz 提到 144 Hz（`g_expectedHz`），面板不支持时由合成器钳制。
- 拦截器按 `OH_Input_GetTouchEventToolType` 过滤非手指工具（笔/橡皮/画笔等），这些事件的 Down 不喂给引擎，后续 Move/Up 自然被忽略，不会留下幽灵触点；新增 `INTERCEPT_TOOL_DROP` 与 `INTERCEPT_TOOLS finger/other` 计数。
- 只做了命令行 Debug 构建验证，**未做真机验证**。

## 4.1.0

- 适配 Phigros Android 4.1.0，HAP 版本名和内部版本号与原 APK 一致。
- 首次启动导入原 APK。未压缩条目直接从原包读取，需要解压的条目在启动前写入缓存并校验。
- 连续两次返回可打开适配选项，调整独占触屏、帧率显示和刷新率申请。TapTap 授权和键盘使用新版界面。
- 已完成 DevEco Debug 构建、APK 资源检查和未签名 HAP 检查。真机运行待验证。

## 1.0.6

- 接入真实 Ability 前后台事件，在 Unity 主线程调用原生暂停/恢复及焦点回调。
- 暂停期间停止渲染，清理活动触摸；支持窗口撤回和恢复，保留快速切换的后台边沿。
- 当前平板测试者确认异象 PV 返回卡死及谱面不自动暂停问题解决。

## 1.0.5

- 接入真实 PV 解码和独立视频窗口，修复视频首帧路由到主窗口。
- 输入法、原生提示框与原游戏退出回调桥接；321 结尾确认后退出经测试者确认。
- 实际显示刷新率与最高120Hz申请；JNI 方法查询索引。

## 1.0.4

- 修复原 Unity 线程任务回调的执行顺序，改善云存档恢复。

以上为原转译层测试记录。首次源码发布另有工程清理与从源码构建兼容库，构建验证不等同于再次通过所有游戏流程。
