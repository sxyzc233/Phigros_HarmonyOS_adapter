import probe from 'libtransprobe.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { TapSdkBridge, SdkCall } from '../probe/TapSdkBridge';
import { TapAuthFlow, TapAuthView } from '../probe/TapAuthFlow';
import { TapLoginFlow } from '../probe/TapLoginFlow';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { originalApkReady, existingApkCompatible, importOriginalApk, downloadOriginalApk } from '../probe/OriginalApk';
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

interface AssetPreparationStatus {
  active: boolean; done: number; total: number; bytes: number; bytesTotal: number;
}

interface InputInterceptResult {
  ok: boolean; result: number;
}

interface FrameStats {
  fps: number; frames: number;
}

/* The native render loop's current frame, how long it has been running (0 = no frame in flight) and
 * how many frames have completed since start. */
interface FrameHealth {
  serial: number; stuckMs: number; frames: number;
}

/* Window for the second back press; long enough for a deliberate double press, short enough to
 * not feel like the first press was remembered. */
const DOUBLE_BACK_MS: number = 1500;

/* Investigation switch: start with the detailed trace already on (see aboutToAppear). */
const TRACE_ON_START: boolean = false;

/*
 * The sheet's close transition owns the bound flag for a moment after it starts. Opening inside that
 * window is cancelled by the framework's own write, so requests wait it out instead of flashing.
 */
/*
 * The close transition owns the bound flag for its whole run: measured from the logs, onWillDisappear
 * to onDisappear is ~590 ms. Opening inside that window creates a second sheet instance, and when the
 * old one finishes tearing down it writes the bound flag back to false and kills the new one - the
 * flash-then-dead behaviour. Requests wait the window out instead.
 */
const SHEET_SETTLE_MS: number = 700;
const SHEET_SETTLE_POLL_MS: number = 80;
const SHEET_RETRY_DELAY_MS: number = 120;
const SHEET_OPEN_RETRIES: number = 3;
const SHEET_PENDING_NONE: number = 0;
const SHEET_PENDING_OPTIONS: number = 1;
const SHEET_PENDING_AUTH: number = 2;

/*
 * A frame the engine has been inside for this long is dead, not slow: healthy frames on this device
 * run 7-25 ms, so the margin is two orders of magnitude.
 */
const FRAME_STUCK_MS: number = 5000;
/*
 * Frames that must have completed before the watchdog is allowed to judge anything. A first frame that
 * spends a long time preparing assets is normal start-up, and so is a short run of fast frames followed
 * by one very heavy one - both used to be mistaken for a deadlock and restarted the process.
 * 512 frames is roughly four seconds at 120 Hz (more at 60), i.e. "the game is really running now".
 */
const FRAME_STUCK_ARM_FRAMES: number = 512;
const FRAME_STUCK_RESTART_ENABLED: boolean = true;

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

  /*
   * This title's in-game restart is a pure .NET/Unity restart: it asks the platform for nothing
   * (verified on device - no Intent, no getLaunchIntentForPackage, no startActivity, no killProcess),
   * it saves the whole PlayerPrefs store, reloads it inside one frame, and then that frame never
   * returns - the engine blocks inside libunity waiting for a graphics device our host does not
   * rebuild. There is no platform hook to implement, so the honest fix is to notice the dead frame
   * and hand the player what Android hands them: a real restart of the application.
   */
  private pollFrameHealth(): void {
    if (this.restartInProgress || this.exitInProgress) return;
    const health = probe.frameHealth() as FrameHealth;
    /* Not enough frames yet: still starting up (asset preparation, first loads), which is not a hang. */
    if (health.frames < FRAME_STUCK_ARM_FRAMES) return;
    if (!this.watchdogArmedLogged) {
      this.watchdogArmedLogged = true;
      hilog.info(0x0000, 'TransProbe', 'FRAME_WATCHDOG_ARMED frames=%{public}s', String(health.frames));
    }
    if (health.stuckMs < FRAME_STUCK_MS) return;
    const serial = String(health.serial);
    if (serial !== this.stuckLoggedSerial) {
      this.stuckLoggedSerial = serial;
      hilog.warn(0x0000, 'TransProbe', 'FRAME_STUCK serial=%{public}s stuckMs=%{public}u frames=%{public}s restart=%{public}s',
        serial, health.stuckMs, String(health.frames), FRAME_STUCK_RESTART_ENABLED ? 'on' : 'off');
    }
    if (!FRAME_STUCK_RESTART_ENABLED) return;
    this.restartInProgress = true;
    hilog.warn(0x0000, 'TransProbe', 'FRAME_STUCK_RESTART serial=%{public}s stuckMs=%{public}u',
      serial, health.stuckMs);
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const info = context.abilityInfo;
    try {
      context.getApplicationContext().restartApp({
        bundleName: info.bundleName,
        moduleName: info.moduleName,
        abilityName: info.name
      });
    } catch (error) {
      this.restartInProgress = false;
      hilog.error(0x0000, 'TransProbe', 'FRAME_STUCK_RESTART failed %{public}s', JSON.stringify(error));
    }
  }

  /*
   * The engine cannot open a browser by itself: Application.OpenURL turns into Intent.ACTION_VIEW
   * plus startActivity, which the native layer now parks in a one-slot queue. openLink is the
   * HarmonyOS equivalent, and it also routes non-http schemes to whatever handler owns them.
   */
  private pollExternalOpen(): void {
    const url = probe.takeExternalOpen() as string;
    if (!url) return;
    hilog.info(0x0000, 'TransProbe', 'EXTERNAL_OPEN url=%{public}s', url);
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    context.openLink(url).catch((error: Error) => {
      hilog.error(0x0000, 'TransProbe', 'EXTERNAL_OPEN failed %{public}s', JSON.stringify(error));
    });
  }

  private pollKeyboard(): void {    if (!this.exitInProgress && probe.exitRequested()) {
      this.exitInProgress = true;
      // Honor the original game's own quit request (including the 321 ending).
      // Close our UIAbility before ending our process, so reopening is a fresh boot.
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      context.terminateSelf().then((): void => { probe.finishRequestedExit(); }).catch((): void => { probe.finishRequestedExit(); });
      return;
    }
    this.pollNativeDialog();
    this.pollFrameHealth();
    this.pollExternalOpen();
    /* Cheap here and always runs, so a keyboard or dialog appearing drops the interceptor at once. */
    this.applyInputIntercept();
    /* Frame rate is sampled by the native render loop once a second; read it a little more often. */
    if (this.showFps && Date.now() - this.fpsPolledAt >= 400) {
      this.fpsPolledAt = Date.now();
      const stats = probe.frameStats() as FrameStats;
      this.fpsValue = stats.fps;
    }
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
    // A back press while the sheet is up is a programmatic close (no gesture in flight), so the page
    // is allowed to write the flag here - but not while a previous dismissal is still settling, which
    // is exactly the window a quick double press lands in.
    if (this.sheetVisible && this.sheetClosingUntil <= Date.now()) {
      hilog.info(0x0000, 'TransProbe', 'SHEET close_by_back');
      this.wantSheet = SHEET_PENDING_NONE;
      this.sheetVisible = false;
      return true;
    }
    // The back gesture never leaves the game: two presses in quick succession open the sheet
    // instead, so a single stray swipe cannot drop the player out mid-song.
    const now: number = Date.now();
    if (this.lastBackPress > 0 && now - this.lastBackPress <= DOUBLE_BACK_MS) {
      this.lastBackPress = 0;
      hilog.info(0x0000, 'TransProbe', 'SHEET request=options source=double_back visible=%{public}d',
        this.sheetVisible ? 1 : 0);
      this.openOptionsSheet();
      return true;
    }
    this.lastBackPress = now;
    return true;
  }
  private surfaceBound: boolean = false;
  /*
   * Single owner of the sheet's open state. The page only writes true to open it (and false for the
   * one programmatic close, when the authorisation flow finishes on its own); a dismissal driven by
   * the player is owned by the framework through $$ and the page must not write back, otherwise the
   * in-flight transition restarts from the centre. The previous design had a derived flag plus
   * watchers, which is exactly that forbidden second write.
   */
  @State @Watch('onSheetVisibilityChanged') sheetVisible: boolean = false;
  /*
   * Which card this sheet session shows, frozen while it is up. SheetOptions has no "closed"
   * callback, so state that is cleared on dismissal would swap the card mid-animation.
   */
  @State sheetIsAuth: boolean = false;
  /* Until this timestamp the close transition still owns the flag; see requestSheet(). */
  private sheetClosingUntil: number = 0;
  /* One-shot latch: the intercept permission is requested at most once per process. */
  private interceptPermissionSettled: boolean = false;
  private pendingSheet: number = SHEET_PENDING_NONE;
  /* What the page currently wants on screen, and how many times an open has been re-issued. */
  private wantSheet: number = SHEET_PENDING_NONE;
  private openAttempts: number = 0;
  private openedAt: number = 0;
  /* Set when the authorisation flow hides its own card, so the close is not mistaken for a cancel. */
  private authClosedByFlow: boolean = false;
  /*
   * Heading strings for the current card. Kept as state so a re-render hands the sheet the same
   * values instead of a freshly built literal, which would also disturb a running transition.
   */
  @State sheetTitle: string = 'Phigros 选项';
  @State sheetSubtitle: string = '快速进行 2 次返回操作可打开此页面';

  private isOptionsSheet(): boolean { return this.sheetVisible && !this.sheetIsAuth; }
  private isAuthSheet(): boolean { return this.sheetVisible && this.sheetIsAuth; }
  /*
   * Exclusive touch starts on. The window path only ever hands the engine one consolidated state per
   * frame - the frame's other touch points are not exposed to the app at all (GetHistoricalPoints
   * comes back empty on this device) - so multi-finger fast taps lose presses. The interceptor reports
   * every finger's down/move/up as its own event, which is what the game needs. The switch stays in
   * the options sheet so it can be given back to the window path.
   */
  @State inputInterceptWanted: boolean = true;
  @State inputInterceptActive: boolean = false;
  @State inputInterceptNote: string = '';
  @State showFps: boolean = false;
  /* Result line shown to the right of the high-refresh button, so the outcome is visible next to it. */
  @State frameRateNote: string = '当前请求 120Hz';
  @State jniTrace: boolean = false;
  /*
   * Touch shaping switches. v11 turns the three that touch the reported position back OFF by default.
   * A predicted position is a guess about where the finger will be: on a hold note that guess can sit
   * outside the note while the real finger is still on it, which the engine reads as the hold breaking.
   * Measured moves are the honest signal, so by default the engine now gets exactly what the panel
   * reported. The switches stay here for a device that benefits from them.
   */
  @State optPredict: boolean = false;
  @State optJitter: boolean = false;
  /*
   * The merge is a *frame* merge now: the moves already queued are folded into one MotionEvent carrying
   * every pointer, which is exactly what a real device sends. It does not rewrite any coordinate, and it
   * is the difference between one engine call per input frame and one per finger per frame.
   */
  @State optCoalesce: boolean = true;
  @State optDupeRecover: boolean = true;
  @State optToolFilter: boolean = true;
  @State optHighRate: boolean = true;
  private restartInProgress: boolean = false;
  /* Serial of the last stalled frame we already reported, so one stall logs once, not every poll. */
  private stuckLoggedSerial: string = '';
  /* One-shot record of the moment the watchdog became allowed to judge, for log-based diagnosis. */
  private watchdogArmedLogged: boolean = false;
  @State fpsValue: number = 0;
  private fpsPolledAt: number = 0;
  private lastBackPress: number = 0;
  private sdk: TapSdkBridge = new TapSdkBridge();
  private auth: TapAuthFlow = new TapAuthFlow(this.sdk);
  private account: TapLoginFlow = new TapLoginFlow(this.sdk, this.auth);
  private activeAuth: string = 'anti';
  @State sdkTitle: string = 'TapTap 认证';
  @State sdkStatus: string = '';
  @State authQr: string = '';
  @State authBusy: boolean = false;
  @State resourcesReady: boolean = false;
  @State assetFailure: string = '';
  @State preparingAssets: boolean = false;
  @State preparePercent: number = 0;
  @State prepareText: string = '';
  private prepareTimer: number = -1;
  private assetsPrepared: boolean = false;
  private assetPrepareInFlight: boolean = false;
  @State importBusy: boolean = false;
  @State importStatus: string = '首次使用需要导入 Phigros ' + APK_PROFILE_ID + ' Android 原包';
  @State importFailed: boolean = false;
  @State showApkLink: boolean = false;
  @State apkLink: string = '';

  aboutToAppear(): void {
    /*
     * Temporary: the trace has to be on before the engine seizes up, and the sheet cannot be opened
     * once it has, so the investigation build turns it on by itself. Flip this to false and the
     * switch in 调试 is the only way in.
     */
    if (TRACE_ON_START) probe.setJniTrace(true);
    /* Build identity, ArkTS half: printed by the .abc, so a log line here proves the bundle itself was
     * replaced - which the native half alone cannot distinguish. */
    hilog.info(0x0000, 'TransProbe', 'TOUCH_BUILD_ETS v12');
    this.jniTrace = TRACE_ON_START;
    this.applyTouchOptions();
    if (this.optHighRate) this.requestHighFrameRate();
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
      this.sdkStatus = view.message; this.authQr = view.qr; this.authBusy = view.busy;
      this.applyAuthView(view.visible);
    };
    this.account.onView = (view: TapAuthView) => {
      this.activeAuth = 'login'; this.sdkTitle = 'TapTap 账号登录';
      this.sdkStatus = view.message; this.authQr = view.qr; this.authBusy = view.busy;
      this.applyAuthView(view.visible);
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
      this.finishApkImport();
    } catch (error) {
      this.importFailed = true;
      this.importStatus = error instanceof Error ? error.message : '未能导入资源，请检查文件和剩余存储空间后重试。';
    } finally { this.importBusy = false; }
  }

  private finishApkImport(): void {
    this.assetsPrepared = false;
    this.resourcesReady = true;
    this.startUnity();
  }

  private async downloadApk(): Promise<void> {
    if (this.importBusy) return;
    this.importBusy = true;
    this.importFailed = false;
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      await downloadOriginalApk(context, this.apkLink, (message: string) => { this.importStatus = message; });
      this.apkLink = '';
      this.finishApkImport();
    } catch (error) {
      this.importFailed = true;
      this.importStatus = error instanceof Error ? error.message : '下载未完成，请检查 HTTPS 直链后重试。';
    } finally { this.importBusy = false; }
  }

  private openTapTapDownload(): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    context.openLink('https://www.taptap.cn/app/165287').catch(() => {
      this.importStatus = '无法打开 TapTap，请在 TapTap 搜索 Phigros。';
    });
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

  /*
   * The asset cache is completed before the engine starts, never lazily. Native verifies the
   * manifest written by a previous run and re-extracts everything only when it is missing, stale or
   * incomplete; the poll below only reaches the screen when that re-extraction actually runs, so a
   * healthy start still goes from the black surface straight into the game.
   */
  private pollPreparation(): void {
    const status = probe.assetPreparationStatus() as AssetPreparationStatus;
    if (status === undefined || status === null) return;
    this.preparingAssets = status.active;
    if (status.total <= 0) return;
    const percent: number = Math.floor(status.done * 100 / status.total);
    this.preparePercent = percent > 100 ? 100 : percent;
    this.prepareText = '已准备 ' + status.done.toString() + ' / ' + status.total.toString() +
      '（' + this.preparePercent.toString() + '%，' + Math.floor(status.bytes / 1000000).toString() +
      ' / ' + Math.floor(status.bytesTotal / 1000000).toString() + ' MB）';
  }

  private async prepareAssets(): Promise<void> {
    if (this.assetsPrepared || this.assetPrepareInFlight) return;
    this.assetPrepareInFlight = true;
    this.assetFailure = '';
    this.preparingAssets = false;
    this.preparePercent = 0;
    this.prepareText = '';
    this.prepareTimer = setInterval(() => this.pollPreparation(), 200);
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const configured: boolean = probe.configureAssetCache(context.cacheDir) as boolean;
      if (!configured) {
        this.assetFailure = '资源准备失败：缓存目录不可用，请检查剩余空间后重启。';
        return;
      }
      const ready: boolean = await (probe.prepareImportedAssets() as Promise<boolean>);
      if (!ready) {
        this.assetFailure = '资源准备失败：请检查空间，或重新选择完整的' + APK_PROFILE_ID + ' APK。';
        return;
      }
      this.assetsPrepared = true;
      this.startUnity();
    } catch (error) {
      hilog.error(0x0000, 'TransAsset', 'ASSET_PREPARE_THREW %{public}s', JSON.stringify(error));
      this.assetFailure = '资源准备失败：请检查空间，或重新选择完整的' + APK_PROFILE_ID + ' APK。';
    } finally {
      if (this.prepareTimer >= 0) { clearInterval(this.prepareTimer); this.prepareTimer = -1; }
      this.preparingAssets = false;
      this.assetPrepareInFlight = false;
    }
  }

  private startUnity(): void {
    if (this.started || !this.resourcesReady || !this.surfaceBound) {
      return;
    }
    if (!this.assetsPrepared) {
      this.prepareAssets();
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
    /*
     * ArkUI runs onTouch at most once per frame and reports only the frame's last touch event; the
     * others become historical points. changedTouches alone therefore cannot describe a frame in which
     * two fingers changed, so the whole pressed set (touches) is sent and the native layer reconciles
     * it against what the engine holds. Native coordinates are component-local pixels, hence vp2px.
     */
    const changed: TouchObject[] = event.changedTouches;
    const changedId: number = changed.length > 0 ? changed[0].id : -1;
    let changedX: number = 0;
    let changedY: number = 0;
    if (changed.length > 0) {
      changedX = this.getUIContext().vp2px(changed[0].x);
      changedY = this.getUIContext().vp2px(changed[0].y);
    }
    const ids: number[] = [];
    const xs: number[] = [];
    const ys: number[] = [];
    for (const point of event.touches) {
      ids.push(point.id);
      xs.push(this.getUIContext().vp2px(point.x));
      ys.push(this.getUIContext().vp2px(point.y));
    }
    probe.touchSnapshot(action, changedId, changedX, changedY, ids, xs, ys, event.timestamp);
  }

  /*
   * The sheet is an overlay, not a background transition: the ability stays in the foreground, so
   * nothing else tells the engine that the player can no longer type or tap into it. Forward it as
   * UnityPlayer's own focus change, both directions, from the single place the state changes.
   */
  /*
   * Opening is deferred while a dismissal is still settling. The framework writes the bound flag back
   * when its close transition finishes, and that write lands *after* the page has already opened the
   * next sheet - the new one shows for a frame and is then forced shut, which is the flash-then-dead
   * behaviour. Requests that arrive in that window are remembered and replayed once it passes.
   */
  private requestSheet(auth: boolean): void {
    if (this.sheetClosingUntil > Date.now()) {
      this.pendingSheet = auth ? SHEET_PENDING_AUTH : SHEET_PENDING_OPTIONS;
      this.replayPendingSheet();
      return;
    }
    this.pendingSheet = SHEET_PENDING_NONE;
    this.showSheet(auth);
  }

  private showSheet(auth: boolean): void {
    this.wantSheet = auth ? SHEET_PENDING_AUTH : SHEET_PENDING_OPTIONS;
    this.openAttempts = 0;
    this.openedAt = Date.now();
    this.sheetIsAuth = auth;
    if (auth) {
      this.sheetTitle = this.sdkTitle;
      this.sheetSubtitle = '';
    } else {
      this.sheetTitle = 'Phigros 选项';
      this.sheetSubtitle = '快速进行 2 次返回操作可打开此页面';
    }
    hilog.info(0x0000, 'TransProbe', 'SHEET open kind=%{public}s attempt=%{public}d',
      auth ? 'auth' : 'options', this.openAttempts);
    this.sheetVisible = true;
  }

  private replayPendingSheet(): void {
    const kind: number = this.pendingSheet;
    if (kind === SHEET_PENDING_NONE) return;
    setTimeout(() => {
      if (this.pendingSheet !== kind) return;
      if (this.sheetClosingUntil > Date.now()) { this.replayPendingSheet(); return; }
      this.pendingSheet = SHEET_PENDING_NONE;
      this.showSheet(kind === SHEET_PENDING_AUTH);
    }, SHEET_SETTLE_POLL_MS);
  }

  private openOptionsSheet(): void { this.requestSheet(false); }

  private openAuthSheet(): void { this.requestSheet(true); }

  /*
   * The authorisation flow reports visibility on its own schedule. Showing it opens the sheet; hiding
   * it is the one programmatic close the page owns (success, failure or an explicit cancel), so it
   * writes the flag only when the card it controls is the one on screen.
   */
  private applyAuthView(visible: boolean): void {
    if (visible) {
      if (!this.sheetVisible || !this.sheetIsAuth) this.openAuthSheet();
      return;
    }
    if (this.sheetIsAuth) {
      hilog.info(0x0000, 'TransProbe', 'SHEET close_by_flow');
      // The flow is closing its own card - success, failure or its own cancel. This must NOT be
      // treated as the player giving up: doing so used to cancel a successful login right after it
      // stored its token, which the game then reported as 80080 GetProfile Failed.
      this.authClosedByFlow = true;
      this.wantSheet = SHEET_PENDING_NONE;
      this.sheetVisible = false;
    }
  }

  /*
   * Sheet visibility. The page writes it to open and to close on its own account; the framework writes
   * it when the player dismisses. If the page still wants a card and the framework reports it shut,
   * that was the tail of an earlier transition swallowing the request - re-issue it a few times, which
   * is what makes a quick close-then-open land instead of flashing once and dying.
   */
  private onSheetVisibilityChanged(propertyName: string): void {
    probe.setAppFocus(!this.sheetVisible);
    this.applyInputIntercept();
    hilog.info(0x0000, 'TransProbe', 'SHEET visible=%{public}d want=%{public}d attempt=%{public}d',
      this.sheetVisible ? 1 : 0, this.wantSheet, this.openAttempts);
    if (this.sheetVisible) {
      this.sheetClosingUntil = 0;
      this.openAttempts = 0;
      return;
    }
    this.sheetClosingUntil = Date.now() + SHEET_SETTLE_MS;
    if (this.pendingSheet !== SHEET_PENDING_NONE) this.replayPendingSheet();

    /*
     * The flag went false without the page asking, so this is the player closing the card. Closing the
     * authorisation card by hand means giving up on the flow, which has to reach the SDK as a
     * cancellation rather than just hiding the card.
     */
    const wasWanted: boolean = this.wantSheet !== SHEET_PENDING_NONE;
    this.wantSheet = SHEET_PENDING_NONE;
    /*
     * Cancel only when the player closed the card. A flow that closed itself has already dealt with
     * its credentials, and cancelling it there would throw away the token it just obtained.
     */
    const closedByFlow: boolean = this.authClosedByFlow;
    this.authClosedByFlow = false;
    if (this.sheetIsAuth && !closedByFlow) {
      hilog.info(0x0000, 'TransProbe', 'SHEET cancel_after_close');
      if (this.activeAuth === 'login') this.account.cancel(); else this.auth.cancel();
    }

    /*
     * A wanted card that the framework reports shut is the tail of an earlier transition swallowing
     * the open (it happens when a reopen lands while the previous teardown is still running).
     * Re-issue it once the window has passed.
     */
    if (!wasWanted || this.openAttempts >= SHEET_OPEN_RETRIES) {
      if (wasWanted) hilog.warn(0x0000, 'TransProbe', 'SHEET open abandoned');
      return;
    }
    ++this.openAttempts;
    this.scheduleReopen(this.sheetIsAuth ? SHEET_PENDING_AUTH : SHEET_PENDING_OPTIONS);
  }

  /*
   * Waits out whatever is left of the close transition, then opens. Polling instead of a fixed delay
   * because the teardown length is the framework's business, not ours.
   */
  private scheduleReopen(kind: number): void {
    setTimeout(() => {
      if (this.sheetVisible || this.wantSheet !== kind) return;
      if (this.sheetClosingUntil > Date.now()) { this.scheduleReopen(kind); return; }
      hilog.info(0x0000, 'TransProbe', 'SHEET reopen attempt=%{public}d', this.openAttempts);
      this.showSheet(kind === SHEET_PENDING_AUTH);
    }, SHEET_RETRY_DELAY_MS);
  }

  /*
   * Single source of truth for "the player is actually looking at the game". The sheet, the import
   * and asset-preparing screens, TapTap authorisation, the IME and the game's own native dialog all
   * count as NOT the game screen.
   */
  private inGameScreen(): boolean {
    const failed: boolean = this.unityReport.startsWith('兼容性检查未通过') ||
      this.unityReport.startsWith('unity threw:');
    return this.started && this.surfaceBound && this.resourcesReady && !failed &&
      !this.sheetVisible && !this.nativeDialogVisible && !this.keyboardVisible;
  }

  /*
   * Exclusive touch must never outlive the game screen: while it is installed the window receives no
   * touch at all, so a panel opened over the game could not be operated and the switch could never
   * be turned back off. The interceptor therefore follows "wanted AND on the game screen", and is
   * only touched when that value actually changes.
   */
  private applyInputIntercept(): void {
    const wanted: boolean = this.inputInterceptWanted && this.inGameScreen();
    if (wanted === this.inputInterceptActive) return;

    if (!wanted) {
      probe.stopInputIntercept();
      this.inputInterceptActive = false;
      return;
    }

    /*
     * INTERCEPT_INPUT_EVENT is a user grant. Ask for it the first time the game screen wants the
     * interceptor, then never again: a denial turns the switch off inside toggleInputIntercept, so
     * this cannot turn into a permission prompt loop.
     */
    if (!this.interceptPermissionSettled) {
      this.interceptPermissionSettled = true;
      this.toggleInputIntercept(true);
      return;
    }

    const state = probe.startInputIntercept() as InputInterceptResult;
    this.inputInterceptActive = state.ok;
    if (!state.ok) this.inputInterceptNote = '启动失败，返回码 ' + state.result.toString();
    hilog.info(0x0000, 'TransInput', 'INTERCEPT_START ok=%{public}d result=%{public}d',
      state.ok ? 1 : 0, state.result);
  }

  /*
   * Asks the compositor for a 144 Hz range. The panel decides: 0 means it accepted the range, -2 means
   * no surface yet (the request is remembered and applied on the next surface), anything else is the
   * platform refusing it - in which case the previous rate stays and the note says so instead of
   * pretending the switch worked.
   */
  /*
   * Pushes the native touch switches. Called once at startup and on every toggle, so the sheet and the
   * engine's actual behaviour cannot drift apart. Each switch is read per event natively, so a change
   * applies to the next touch rather than the next launch.
   */
  private applyTouchOptions(): void {
    probe.setTouchOption('predict', this.optPredict);
    probe.setTouchOption('jitter', this.optJitter);
    probe.setTouchOption('coalesce', this.optCoalesce);
    probe.setTouchOption('dupeRecover', this.optDupeRecover);
    probe.setTouchOption('toolFilter', this.optToolFilter);
  }

  private requestHighFrameRate(): void {
    let result: number = -1;
    try {
      result = probe.requestFrameRate(144) as number;
    } catch (error) {
      hilog.error(0x0000, 'TransProbe', 'FRAME_RATE_REQUEST hz=144 failed %{public}s', JSON.stringify(error));
      this.frameRateNote = '申请失败（接口不可用）';
      return;
    }
    hilog.info(0x0000, 'TransProbe', 'FRAME_RATE_REQUEST hz=144 result=%{public}d', result);
    if (result === 0) {
      this.frameRateNote = '已申请 144Hz';
    } else if (result === -2) {
      this.frameRateNote = '已记录，画面就绪后生效';
    } else {
      this.frameRateNote = '系统不接受 144Hz（' + result + '）';
    }
  }

  /*
   * The switch records intent only; applyInputIntercept() decides whether the interceptor may
   * actually be installed right now.
   */
  private async toggleInputIntercept(on: boolean): Promise<void> {
    if (!on) {
      this.inputInterceptWanted = false;
      this.inputInterceptNote = '';
      this.applyInputIntercept();
      return;
    }
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const permissions: Array<Permissions> = ['ohos.permission.INTERCEPT_INPUT_EVENT'];
      const outcome = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, permissions);
      const granted: boolean = outcome.authResults.length > 0 && outcome.authResults[0] === 0;
      if (!granted) {
        this.inputInterceptWanted = false;
        this.inputInterceptNote = '未授予权限，无法独占触屏。';
        hilog.warn(0x0000, 'TransInput', 'INTERCEPT_PERMISSION_DENIED');
        return;
      }
      this.inputInterceptWanted = true;
      this.applyInputIntercept();
    } catch (error) {
      this.inputInterceptWanted = false;
      this.inputInterceptNote = '启动异常：' + JSON.stringify(error);
      hilog.error(0x0000, 'TransInput', 'INTERCEPT_THREW %{public}s', JSON.stringify(error));
    }
  }

  /*
   * Whichever card is logically open. The authorisation card wins if both are somehow set, because it
   * is the one the game is blocked on.
   */
  @Builder
  ActiveSheetContent() {
    if (this.sheetIsAuth) {
      this.AuthSheetContent()
    } else {
      this.OptionsSheetContent()
    }
  }

  /*
   * The TapTap broker's own card, drawn as the same centred sheet the options use. The heading and the
   * close button belong to the framework (title and showClose), so only the live part is here: what
   * the flow is doing right now, the QR the player has to scan, and the two ways out.
   */
  @Builder
  AuthSheetContent() {
    Column({ space: 12 }) {
      Text(this.sdkStatus).fontSize(14).fontColor('#A9B2C0').width('100%')

      if (this.authQr.length > 0) {
        QRCode(this.authQr).width(180).height(180).backgroundColor(Color.White).padding(8)
          .alignSelf(ItemAlign.Center)
        Button('打开 TapTap 官方授权页')
          .width('100%')
          .onClick(() => this.openAuthorization())
      }

      if (!this.authBusy) {
        Button('继续')
          .width('100%')
          .onClick(() => { if (this.activeAuth === 'login') this.account.begin(); else this.auth.begin(); })
      }

      Button('取消')
        .width('100%')
        .backgroundColor('#46505E')
        .onClick(() => { if (this.activeAuth === 'login') this.account.cancel(); else this.auth.cancel(); })

      Text('授权在 TapTap 官方页面完成')
        .fontSize(12).fontColor('#7C8798').width('100%').textAlign(TextAlign.Center)
    }
    .width('100%')
    .padding({ left: 24, right: 24, bottom: 24 })
  }

  /*
   * Centred sheet, because the game runs in landscape: BOTTOM would just be a line across the
   * screen. The header and the close button are the framework's own (showClose), so nothing is
   * drawn by hand and the transition is the default one.
   */
  @Builder
  OptionsSheetContent() {
    /* The panel is a full-height side sheet, so the list owns the whole body height and scrolls
     * inside it: every option keeps its one-line explanation and nothing is clipped. */
    Scroll() {
      Column({ space: 10 }) {
        Text('触控').fontSize(13).fontColor('#7C8798').width('100%')

      Row() {
        Column({ space: 4 }) {
          Text('独占触屏').fontSize(16).fontColor('#E5EAF1')
          Text('开启后系统触摸将由游戏画面独占，类似引导式访问，能够避免一些断触。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.inputInterceptWanted })
          .onChange((isOn: boolean) => { this.toggleInputIntercept(isOn); })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      if (this.inputInterceptNote.length > 0) {
        Text(this.inputInterceptNote).fontSize(12).fontColor('#A9B2C0').width('100%')
      }

      Text('触控优化').fontSize(13).fontColor('#7C8798').width('100%').margin({ top: 8 })

      Row() {
        Column({ space: 4 }) {
          Text('触摸预测补偿').fontSize(16).fontColor('#E5EAF1')
          Text('按手指最近的速度把落点推算到下一帧，最多补 6 px。v11 起默认关闭：推算会把落点推到手指前面，长条上容易判成脱手。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.optPredict })
          .onChange((isOn: boolean) => { this.optPredict = isOn; this.applyTouchOptions(); })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row() {
        Column({ space: 4 }) {
          Text('抖动抑制').fontSize(16).fontColor('#E5EAF1')
          Text('手指按住不动时面板仍会报亚像素漂移，小于约 1.2 px 的移动直接丢掉。v11 起默认关闭：慢滑时会把“在动”变成“没动”。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.optJitter })
          .onChange((isOn: boolean) => { this.optJitter = isOn; this.applyTouchOptions(); })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row() {
        Column({ space: 4 }) {
          Text('移动事件合并').fontSize(16).fontColor('#E5EAF1')
          Text('在 4 ms 内合并各指的移动，使用每根手指的最新坐标。落指和抬指仍按原顺序发送。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.optCoalesce })
          .onChange((isOn: boolean) => { this.optCoalesce = isOn; this.applyTouchOptions(); })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row() {
        Column({ space: 4 }) {
          Text('非手指输入过滤').fontSize(16).fontColor('#E5EAF1')
          Text('笔、手掌、指关节这类工具的触点不进引擎，避免留下按住不放的幽灵触点。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.optToolFilter })
          .onChange((isOn: boolean) => { this.optToolFilter = isOn; this.applyTouchOptions(); })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row() {
        Column({ space: 4 }) {
          Text('同 id 抢占自愈').fontSize(16).fontColor('#E5EAF1')
          Text('抬指事件丢失、同一个 id 被新手指复用时，先补一次抬起注入，再把新手指定为按下。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.optDupeRecover })
          .onChange((isOn: boolean) => { this.optDupeRecover = isOn; this.applyTouchOptions(); })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row() {
        Column({ space: 4 }) {
          Text('自动申请 144Hz').fontSize(16).fontColor('#E5EAF1')
          Text('每次进入游戏自动向合成器申请最高刷新率；面板不支持时由系统钳制回上限。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.optHighRate })
          .onChange((isOn: boolean) => {
            this.optHighRate = isOn;
            if (isOn) this.requestHighFrameRate();
          })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Text('显示').fontSize(13).fontColor('#7C8798').width('100%').margin({ top: 8 })

      Row() {
        Text('显示帧率').fontSize(16).fontColor('#E5EAF1').layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.showFps })
          .onChange((isOn: boolean) => { this.showFps = isOn; })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row({ space: 12 }) {
        Button('申请最大 144Hz')
          .fontSize(14).height(36)
          .onClick(() => { this.requestHighFrameRate(); })
        Text(this.frameRateNote).fontSize(12).fontColor('#A9B2C0').layoutWeight(1)
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Text('调试').fontSize(13).fontColor('#7C8798').width('100%').margin({ top: 8 })

      Row() {
        Column({ space: 4 }) {
          Text('详细日志').fontSize(16).fontColor('#E5EAF1')
          Text('逐条记录所有框架调用和每帧渲染的进入/返回，日志量很大，仅短时开启。')
            .fontSize(12).fontColor('#A9B2C0')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.jniTrace })
          .onChange((isOn: boolean) => {
            this.jniTrace = isOn;
            probe.setJniTrace(isOn);
          })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      }
      .width('100%')
      .padding({ left: 20, right: 20, top: 4, bottom: 24 })
    }
    .width('100%')
    .height('100%')
    .scrollBar(BarState.Auto)
    .edgeEffect(EdgeEffect.Spring)
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

      /*
       * Debug readout. Wrapped in a full-size transparent stack so it cannot influence the layout
       * of the page or the surface: only a 100%x100% container is added, and the badge sits in its
       * top-left corner. Hit-test transparent, so it never steals touches from the game.
       */
      if (this.showFps) {
        Stack({ alignContent: Alignment.TopStart }) {
          Text(this.fpsValue.toString() + ' FPS')
            .fontSize(14)
            .fontColor('#7CE8FF')
            .padding({ left: 8, right: 8, top: 4, bottom: 4 })
            .backgroundColor('#99000000')
            .borderRadius(6)
            .margin({ left: 12, top: 10 })
        }
        .width('100%')
        .height('100%')
        .hitTestBehavior(HitTestMode.None)
      }

      if (this.keyboardVisible) {
        /*
         * A bar across the bottom: field on the left, the two actions on the right, no panel behind
         * them. Everything above the bar is empty and tapping it confirms, which is the same as the
         * IME's own done key.
         */
        Column() {
          Column()
            .width('100%')
            .layoutWeight(1)
            .onClick(() => this.replyKeyboard(1))
          Row({ space: 8 }) {
            TextInput({ text: this.keyboardText, placeholder: this.keyboardHint, controller: this.keyboardController })
              .id('reaUnityIme').type(this.keyboardSecure ? InputType.Password : InputType.Normal)
              .maxLength(this.keyboardLimit).enterKeyType(EnterKeyType.Done).height(48).layoutWeight(1)
              /* Own fill and ink: the bar is white, so an unpainted field would vanish into it. */
              .backgroundColor('#F0F1F3').fontColor('#111111').placeholderColor('#8A9099').borderRadius(8)
              .onChange((value: string) => { this.keyboardText = value; this.keyboardStart = value.length; this.keyboardLength = 0; this.replyKeyboard(0); })
              .onTextSelectionChange((start: number, end: number) => { this.keyboardStart = start; this.keyboardLength = end - start; this.replyKeyboard(0); })
              .onSubmit(() => this.replyKeyboard(1))
            Button('取消')
              .backgroundColor(Color.Transparent).borderRadius(0).fontColor('#46505E').height(48)
              .onClick(() => this.replyKeyboard(2))
            Button('确定')
              .backgroundColor(Color.Transparent).borderRadius(0).fontColor('#0A59F7').height(48)
              .onClick(() => this.replyKeyboard(1))
          }
          .width('100%')
          .backgroundColor(Color.White)
          .padding({ left: 20, right: 20, top: 12, bottom: 16 })
        }
        .width('100%')
        .height('100%')
        /* Fades the whole bar in and out with the IME instead of popping it onto the screen. */
        .transition(TransitionEffect.OPACITY.animation({ duration: 160, curve: Curve.EaseOut }))
      }
      if (this.unityReport.startsWith('兼容性检查未通过') || this.unityReport.startsWith('unity threw:')) {
        Text(this.unityReport).fontColor(Color.White).fontSize(16).padding(20).backgroundColor('#CC000000')
      }
      if (!this.resourcesReady) {
        Column({ space: 18 }) {
          Text('欢迎使用 Phigros')
            .fontSize(28).fontColor(Color.White).fontWeight(FontWeight.Bold)
          if (this.importFailed) { Text('导入未完成').fontColor('#FFB5B5').fontSize(18) }
          Text(this.importStatus).fontSize(16).fontColor(this.importFailed ? '#FFB5B5' : '#DEE5EE').textAlign(TextAlign.Center)
          if (this.importBusy) {
            LoadingProgress().width(36).height(36).color(Color.White)
          } else {
            Row({ space: 8 }) {
              Button('选择 Android 原包').onClick(() => { this.chooseOriginalApk(); })
              Button('打开 TapTap').onClick(() => { this.openTapTapDownload(); })
              Button('使用直链').onClick(() => { this.showApkLink = !this.showApkLink; })
            }
            if (this.showApkLink) {
              TextInput({ placeholder: '粘贴 APK HTTPS 直链', text: this.apkLink })
                .maxLength(8192).onChange((value: string) => { this.apkLink = value; }).width('100%')
              Button('下载并导入（约 2.48 GB）').enabled(this.apkLink.trim().length > 0)
                .onClick(() => { this.downloadApk(); })
            }
          }
          Text('游戏中快速按两次返回即可打开应用选项')
            .fontSize(14).fontColor('#A9B2C0').textAlign(TextAlign.Center)
        }.padding(28).width('78%').constraintSize({ maxWidth: 700 })
      }
      if (this.preparingAssets) {
        Column({ space: 14 }) {
          Text('正在解压安装包').fontSize(20).fontColor(Color.White)
          Progress({ value: this.preparePercent, total: 100, type: ProgressType.Linear })
            .width('100%').color('#0A59F7')
          Text(this.prepareText).fontSize(14).fontColor('#DEE5EE').textAlign(TextAlign.Center)
        }.padding(24).width('72%').constraintSize({ maxWidth: 640 })
      }
      if (this.assetFailure.length > 0) {
        Column({ space: 14 }) {
          Text('资源解压失败').fontSize(22).fontColor('#FFB5B5')
          Text(this.assetFailure).fontSize(16).fontColor('#DEE5EE').textAlign(TextAlign.Center)
          Button('重新选择 APK').onClick(() => { this.chooseOriginalApk(); })
        }.padding(24).width('72%').constraintSize({ maxWidth: 640 })
      }
    }
    .width('100%')
    .height('100%')
    /*
     * A single sheet for both panels: two bindSheet calls on one node fight each other and the first
     * one closes as soon as anything re-renders. The title follows whichever card is showing.
     */
    .bindSheet($$this.sheetVisible, this.ActiveSheetContent(), {
      /*
       * Side sheet: the card docks to the right edge of the window at full height instead of sitting
       * in the middle, so the game stays visible on the left while the player reads the options. A
       * side sheet only supports the full-screen height, which is exactly what the list wants.
       */
      preferType: SheetType.SIDE,
      showClose: true,
      dragBar: false,
      height: SheetSize.LARGE,
      width: '44%',
      title: { title: this.sheetTitle, subtitle: this.sheetSubtitle }
      /*
       * No shouldDismiss / onWillDismiss on purpose. Those hooks hand the dismissal to the page at the
       * moment the gesture crosses the threshold, and continuing it requires calling dismiss(), which
       * the framework runs as a programmatic close from the sheet's settled position - the card jumps
       * back to the centre and replays the animation instead of following the finger. Without the
       * hooks the framework owns the whole gesture-driven transition, and the page learns about the
       * close from the bound flag being written back (see onSheetVisibilityChanged).
       */
    })
  }
}
