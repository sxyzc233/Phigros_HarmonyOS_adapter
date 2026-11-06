import probe from 'libtransprobe.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { TapSdkBridge, SdkCall } from '../probe/TapSdkBridge';
import { TapAuthFlow, TapAuthView } from '../probe/TapAuthFlow';
import { TapLoginFlow } from '../probe/TapLoginFlow';
import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { originalApkReady, existingApkCompatible, importOriginalApk } from '../probe/OriginalApk';
import { APK_PROFILE_ID } from '../probe/ApkProfile';

/*
 * The documented way to learn a surface id is to override the controller's lifecycle methods.
 * Assigning them is rejected by ArkTS, and getXComponentSurfaceId() is empty during onLoad, so a
 * subclass is the only reliable route.
 */
class ReaSurfaceController extends XComponentController {
  onReady: (surfaceId: string) => void = () => {};

  onSurfaceCreated(surfaceId: string): void {
    this.onReady(surfaceId);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
  }

  onSurfaceDestroyed(surfaceId: string): void {
    probe.clearSurface(surfaceId);
  }
}

@CustomDialog
struct ReaNativePopup {
  controller?: CustomDialogController;
  title: string = '';
  message: string = '';
  buttons: string[] = [];
  select: (index: number) => void = (_: number): void => {};

  build() {
    Column({ space: 16 }) {
      Text(this.title).fontSize(22).fontColor('#111111').fontWeight(FontWeight.Medium).width('100%')
      Scroll() { Text(this.message).fontSize(16).fontColor('#333333').width('100%') }
        .constraintSize({ maxHeight: 280 }).width('100%')
      Row({ space: 10 }) {
        ForEach(this.buttons, (label: string, index: number) => {
          Button(label).fontColor('#0A59F7').backgroundColor(Color.Transparent).onClick(() => {
            this.select(index); this.controller?.close();
          })
        })
      }.width('100%').justifyContent(FlexAlign.End)
    }.padding(24).width(420).constraintSize({ maxWidth: '88%' }).backgroundColor('#F5F5F5').borderRadius(20)
  }
}

interface UnityNativeDialogState {
  revision: number; session: number; visible: boolean; cancelable: boolean;
  kind: number; title: string; message: string; buttons: string[];
}

interface UnityKeyboardState {
  revision: number; session: number; visible: boolean; secure: boolean; multiline: boolean;
  type: number; limit: number; start: number; length: number; text: string; hint: string;
}

@Entry
@Component
struct ProbePage {
  @State report: string = AppStorage.get<string>('probeReport') ?? 'probe did not run';
  @State unityReport: string = '';
  @State surfaceState: string = 'waiting for surface';
  private controller: ReaSurfaceController = new ReaSurfaceController();
  private started: boolean = false;
  private keyboardTimer: number = -1;
  private keyboardRevision: number = 0;
  private exitInProgress: boolean = false;
  private nativeDialogRevision: number = 0;
  private nativeDialogController?: CustomDialogController;
  private nativeDialogVisible: boolean = false;

  private pollNativeDialog(): void {
    const state = probe.dialogState(this.nativeDialogRevision) as UnityNativeDialogState | null;
    if (!state) return;
    this.nativeDialogRevision = state.revision;
    this.nativeDialogController?.close();
    this.nativeDialogVisible = state.visible;
    if (!state.visible) return;
    this.nativeDialogController = new CustomDialogController({
      builder: ReaNativePopup({ title: state.title, message: state.message, buttons: state.buttons,
        select: (index: number): void => {
          probe.dialogReply(state.session, index); this.nativeDialogVisible = false;
        }
      }),
      autoCancel: state.cancelable,
      cancel: (): void => { probe.dialogReply(state.session, -1); this.nativeDialogVisible = false; },
      alignment: DialogAlignment.Center,
      customStyle: true
    });
    this.nativeDialogController.open();
  }
  private keyboardSession: number = 0;
  private keyboardController: TextInputController = new TextInputController();
  @State keyboardVisible: boolean = false;
  @State keyboardText: string = '';
  @State keyboardHint: string = '';
  @State keyboardSecure: boolean = false;
  @State keyboardLimit: number = 65535;
  private keyboardStart: number = 0;
  private keyboardLength: number = 0;

  private pollKeyboard(): void {
    if (!this.exitInProgress && probe.exitRequested()) {
      this.exitInProgress = true;
      // Honor the original game's own quit request (including the 321 ending).
      // Close our UIAbility before ending our process, so reopening is a fresh boot.
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      context.terminateSelf().then((): void => { probe.finishRequestedExit(); }).catch((): void => { probe.finishRequestedExit(); });
      return;
    }
    this.pollNativeDialog();
    const state = probe.keyboardState(this.keyboardRevision) as UnityKeyboardState | null;
    if (!state) return;
    const opening: boolean = state.visible && (!this.keyboardVisible || this.keyboardSession !== state.session);
    this.keyboardRevision = state.revision; this.keyboardSession = state.session;
    this.keyboardText = state.text; this.keyboardHint = state.hint; this.keyboardSecure = state.secure;
    this.keyboardLimit = state.limit > 0 ? state.limit : 65535;
    this.keyboardStart = state.start; this.keyboardLength = state.length; this.keyboardVisible = state.visible;
    if (opening) {
      setTimeout(() => {
        this.getUIContext().getFocusController().requestFocus('reaUnityIme');
        this.keyboardController.setTextSelection(state.start, state.start + state.length);
      }, 50);
    }
  }

  private replyKeyboard(status: number): void {
    probe.keyboardReply(this.keyboardSession, this.keyboardText, this.keyboardStart, this.keyboardLength, status);
    if (status !== 0) { this.keyboardVisible = false; this.keyboardController.stopEditing(); }
  }

  onBackPress(): boolean {
    if (this.nativeDialogVisible) return true;
    if (this.keyboardVisible) { this.replyKeyboard(2); return true; }
    return false;
  }
  private surfaceBound: boolean = false;
  private sdk: TapSdkBridge = new TapSdkBridge();
  private auth: TapAuthFlow = new TapAuthFlow(this.sdk);
  private account: TapLoginFlow = new TapLoginFlow(this.sdk, this.auth);
  private activeAuth: string = 'anti';
  @State sdkTitle: string = 'TapTap 认证';
  @State sdkStatus: string = '';
  @State authVisible: boolean = false;
  @State authQr: string = '';
  @State authBusy: boolean = false;
  @State resourcesReady: boolean = false;
  @State importBusy: boolean = false;
  @State importStatus: string = '首次使用需要导入 Phigros ' + APK_PROFILE_ID + ' Android 原包。';
  @State importFailed: boolean = false;

  aboutToAppear(): void {
    this.keyboardTimer = setInterval(() => this.pollKeyboard(), 32);
    this.resourcesReady = originalApkReady();
    if (!this.resourcesReady) {
      this.importBusy = true;
      existingApkCompatible().then((ready: boolean) => {
        this.resourcesReady = ready; this.importBusy = false;
        if (ready) this.startUnity();
      });
    }
    this.sdk.onCall = (call: SdkCall) => {
      if (call.command.service === 'TDSLoginService') this.account.handle(call); else this.auth.handle(call);
    };
    this.auth.onView = (view: TapAuthView) => {
      this.activeAuth = 'anti'; this.sdkTitle = 'TapTap 认证';
      this.authVisible = view.visible; this.sdkStatus = view.message; this.authQr = view.qr; this.authBusy = view.busy;
    };
    this.account.onView = (view: TapAuthView) => {
      this.activeAuth = 'login'; this.sdkTitle = 'TapTap 账号登录';
      this.authVisible = view.visible; this.sdkStatus = view.message; this.authQr = view.qr; this.authBusy = view.busy;
    };
    this.sdk.start();
    this.controller.onReady = (surfaceId: string) => {
      this.surfaceState = 'surface id ' + surfaceId;
      // Native callbacks normally published the real window already. Do not
      // replace it with a second wrapper for the same SurfaceId.
      if (!probe.nativeSurfaceAvailable()) probe.setSurface(surfaceId);
      this.surfaceBound = probe.nativeSurfaceAvailable();
      this.startUnity();
    };
  }

  aboutToDisappear(): void {
    if (this.keyboardTimer >= 0) clearInterval(this.keyboardTimer);
    this.nativeDialogController?.close();
    if (this.keyboardVisible) this.replyKeyboard(2);
    this.sdk.stop(); this.auth.stop(); this.account.stop();
  }

  private async chooseOriginalApk(): Promise<void> {
    if (this.importBusy) return;
    this.importBusy = true;
    this.importFailed = false;
    this.importStatus = '请选择 Phigros ' + APK_PROFILE_ID + ' 的完整 Android APK。';
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const documents = new picker.DocumentViewPicker(context);
      const files: string[] = await documents.select({ maxSelectNumber: 1, fileSuffixFilters: ['Android 原包|.apk'] });
      if (files.length === 0) return;
      await importOriginalApk(files[0], (message: string) => { this.importStatus = message; });
      this.resourcesReady = true;
      this.startUnity();
    } catch (error) {
      this.importFailed = true;
      this.importStatus = error instanceof Error ? error.message : '未能导入资源，请检查文件和剩余存储空间后重试。';
    } finally { this.importBusy = false; }
  }

  private openAuthorization(): void {
    const address: string = this.activeAuth === 'login' ? this.account.authorizationUrl() : this.auth.authorizationUrl();
    if (address.length === 0) return;
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    context.openLink(address).catch(() => {
      this.sdkStatus = '系统未能打开授权页面，请用手机上的 TapTap 扫描二维码。';
    });
  }

  private runAbiTest(): boolean {
    try {
      const report: string = probe.abitest();
      for (const line of report.split('\n')) {
        if (line.length > 0) {
          hilog.info(0x0000, 'TransAbi', '%{public}s', line);
        }
      }
      return report.includes('ABI_SELFTEST PASS');
    } catch (error) {
      hilog.error(0x0000, 'TransAbi', 'ABI_SELFTEST_THREW %{public}s', JSON.stringify(error));
      return false;
    }
  }

  private startUnity(): void {
    if (this.started || !this.resourcesReady || !this.surfaceBound) {
      return;
    }
    this.started = true;
    if (!this.runAbiTest()) {
      this.unityReport = '兼容性检查未通过，已停止启动原引擎。请查看 TransAbi 日志。';
      return;
    }
    this.unityReport = 'running Unity bootstrap...';
    try {
      const result: string = probe.traced();
      this.unityReport = result.includes('PREFS_DISK_SELFTEST FAIL') || result.includes('JNI_PREFERENCES_CONTRACT FAIL') ?
        '兼容性检查未通过：保存层检查失败，已停止启动游戏，防止进度丢失。' : result;
    } catch (error) {
      this.unityReport = 'unity threw: ' + JSON.stringify(error);
    }
  }

  private routeTouch(event: TouchEvent): void {
    if (probe.nativeTouchAvailable()) return;
    const action: number = event.type === TouchType.Down ? 0 : event.type === TouchType.Up ? 1 :
      event.type === TouchType.Cancel ? 3 : 2;
    for (const point of event.changedTouches) {
      probe.touch(action, point.id, this.getUIContext().vp2px(point.x), this.getUIContext().vp2px(point.y), event.timestamp);
    }
  }

  build() {
    Stack() {
      // The engine renders into this surface. Its id reaches the native layer first, because
      // ANativeWindow_fromSurface must already answer when the engine starts.
      XComponent({
        id: 'reaSurface',
        type: XComponentType.SURFACE,
        libraryname: 'transprobe',
        controller: this.controller
      })
        .width('100%')
        .height('100%')
        .onLoad(() => {
          if (probe.nativeSurfaceAvailable()) { this.surfaceBound = true; this.startUnity(); }
        })
        .onTouch((event: TouchEvent) => this.routeTouch(event))

      if (this.keyboardVisible) {
        Column({ space: 10 }) {
          TextInput({ text: this.keyboardText, placeholder: this.keyboardHint, controller: this.keyboardController })
            .id('reaUnityIme').type(this.keyboardSecure ? InputType.Password : InputType.Normal)
            .maxLength(this.keyboardLimit).enterKeyType(EnterKeyType.Done).height(48).width('100%')
            .onChange((value: string) => { this.keyboardText = value; this.keyboardStart = value.length; this.keyboardLength = 0; this.replyKeyboard(0); })
            .onTextSelectionChange((start: number, end: number) => { this.keyboardStart = start; this.keyboardLength = end - start; this.replyKeyboard(0); })
            .onSubmit(() => this.replyKeyboard(1))
          Row({ space: 18 }) {
            Button('取消').backgroundColor('#46505E').onClick(() => this.replyKeyboard(2))
            Button('确定').onClick(() => this.replyKeyboard(1))
          }
        }.padding(16).width('72%').constraintSize({ maxWidth: 720 }).backgroundColor('#F0151B25').borderRadius(12)
      }
      if (this.unityReport.startsWith('兼容性检查未通过') || this.unityReport.startsWith('unity threw:')) {
        Text(this.unityReport).fontColor(Color.White).fontSize(16).padding(20).backgroundColor('#CC000000')
      }
      if (!this.resourcesReady) {
        Column({ space: 18 }) {
          Text('Phigros · Android 资源导入').fontSize(28).fontColor(Color.White)
          if (this.importFailed) { Text('导入未完成').fontColor('#FFB5B5').fontSize(18) }
          Text(this.importStatus).fontSize(16).fontColor(this.importFailed ? '#FFB5B5' : '#DEE5EE').textAlign(TextAlign.Center)
          Text('请先将APK保存到本机。支持相同游戏内容的不同压缩打包方式；导入时会检查版本并显示进度。')
            .fontSize(13).fontColor('#A9B2C0').textAlign(TextAlign.Center)
          if (this.importBusy) {
            LoadingProgress().width(36).height(36).color(Color.White)
          } else {
            Button('选择 Android 原包').onClick(() => { this.chooseOriginalApk(); })
          }
        }.padding(28).width('78%').constraintSize({ maxWidth: 700 }).backgroundColor('#F0151B25').borderRadius(16)
      }
      if (this.authVisible) {
        Column({ space: 14 }) {
          Text(this.sdkTitle).fontSize(22).fontColor(Color.White).fontWeight(FontWeight.Medium)
          Text(this.sdkStatus).fontColor('#E5EAF1').fontSize(16).textAlign(TextAlign.Center)
          if (this.authQr.length > 0) {
            QRCode(this.authQr).width(180).height(180).backgroundColor(Color.White).padding(8)
            Button('打开 TapTap 官方授权页').onClick(() => this.openAuthorization())
          }
          if (!this.authBusy) {
            Button('继续／重试').onClick(() => { if (this.activeAuth === 'login') this.account.begin(); else this.auth.begin(); })
          }
          Button('取消').backgroundColor('#46505E').onClick(() => { if (this.activeAuth === 'login') this.account.cancel(); else this.auth.cancel(); })
          Text('授权在 TapTap 官方页面完成。此测试版仅在内存中保存凭证。')
            .fontColor('#A9B2C0').fontSize(12).textAlign(TextAlign.Center)
        }.padding(24).backgroundColor('#F0151B25').width('60%').constraintSize({ maxWidth: 640 }).borderRadius(12)
      }
    }
    .width('100%')
    .height('100%')
  }
}
