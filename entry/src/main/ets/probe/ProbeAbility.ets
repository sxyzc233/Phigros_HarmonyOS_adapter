import probe from 'libtransprobe.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import common from '@ohos.app.ability.common';
import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import window from '@ohos.window';
import display from '@ohos.display';
import { i18n } from '@kit.LocalizationKit';
import { connection } from '@kit.NetworkKit';
import { certificateManager } from '@kit.DeviceCertificateKit';
import { fileIo } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { ReaCrashDiagnostics } from './ReaCrashDiagnostics';
import { backupBeforeSyncFix } from './SyncRecoveryBackup';

/**
 * Hosts the Android-on-HarmonyOS translation probe.
 *
 * This ability deliberately imports nothing from tuanjieLib: the goal is to load the ORIGINAL
 * Android libraries, so the Tuanjie engine must not be touched at all.
 */
export default class ProbeAbility extends UIAbility {
  private pageUri: string = 'pages/ProbePage';
  private networkTimer: number = -1;
  private crashDiagnostics: ReaCrashDiagnostics = new ReaCrashDiagnostics();

  private refreshNetwork(): void {
    try { probe.setDisplayRefreshRate(display.getDefaultDisplaySync().refreshRate); } catch (_) {}
    try {
      const net = connection.getDefaultNetSync();
      let type: number = 0;
      if (net.netId > 0) {
        const caps = connection.getNetCapabilitiesSync(net);
        type = caps.bearerTypes.includes(connection.NetBearType.BEARER_CELLULAR) ? 1 : 2;
      }
      probe.setNetworkType(type);
      connection.getDefaultHttpProxy().then(proxy => {
        const value: string = proxy.host.length > 0 ? proxy.host + ':' + proxy.port +
          (proxy.exclusionList.length > 0 ? '\n' + proxy.exclusionList.join('|') : '') : '';
        probe.setSystemProxy(value);
      }).catch((error: Error) => hilog.warn(0x0000, 'TransNetwork', 'PROXY_QUERY_FAILED %{public}s', JSON.stringify(error)));
    } catch (error) {
      probe.setNetworkType(0);
      hilog.warn(0x0000, 'TransNetwork', 'NETWORK_QUERY_FAILED %{public}s', JSON.stringify(error));
    }
  }

  onForeground(): void { probe.setAppForeground(true); this.refreshNetwork(); }
  onBackground(): void { probe.setAppForeground(false); }
  onDestroy(): void { probe.setAppForeground(false); if (this.networkTimer >= 0) clearInterval(this.networkTimer); }

  private loadSystemTrustRoots(): void {
    let loaded: number = 0;
    try {
      const path = certificateManager.getCertificateStorePath({ certType: certificateManager.CertType.CA_CERT_SYSTEM });
      const decoder = new util.Base64Helper();
      for (const name of fileIo.listFileSync(path)) {
        try {
          const text: string = fileIo.readTextSync(path + '/' + name);
          const begin: string = '-----BEGIN CERTIFICATE-----';
          const end: string = '-----END CERTIFICATE-----';
          let start: number = text.indexOf(begin);
          while (start >= 0) {
            const finish: number = text.indexOf(end, start);
            if (finish < 0) break;
            const encoded: string = text.substring(start + begin.length, finish).replace(/\s/g, '');
            probe.addSystemTrustRoot(decoder.decodeSync(encoded));
            loaded++;
            start = text.indexOf(begin, finish + end.length);
          }
        } catch (error) { hilog.warn(0x0000, 'TransNetwork', 'CA_FILE_READ_FAILED %{public}s', JSON.stringify(error)); }
      }
    } catch (error) { hilog.error(0x0000, 'TransNetwork', 'SYSTEM_CA_LOAD_FAILED %{public}s', JSON.stringify(error)); }
    hilog.info(0x0000, 'TransNetwork', 'SYSTEM_CA_LOADED count=%{public}d', loaded);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    backupBeforeSyncFix();
    this.crashDiagnostics.start(this.context.filesDir);
    probe.setSystemLocale(i18n.System.getSystemLocale());
    this.loadSystemTrustRoots();
    this.refreshNetwork();
    this.networkTimer = setInterval(() => this.refreshNetwork(), 2000);
    try {
      const report: string = probe.run();
      hilog.info(0x0000, 'TransProbe', 'PROBE_REPORT_BEGIN');
      for (const line of report.split('\n')) {
        if (line.length > 0) {
          hilog.info(0x0000, 'TransProbe', '%{public}s', line);
        }
      }
      hilog.info(0x0000, 'TransProbe', 'PROBE_REPORT_END');
      AppStorage.setOrCreate<string>('probeReport', report);
    } catch (error) {
      hilog.error(0x0000, 'TransProbe', 'PROBE_THREW %{public}s', JSON.stringify(error));
      AppStorage.setOrCreate<string>('probeReport', 'probe threw: ' + JSON.stringify(error));
    }

    // Second stage: build a fake JVM and let Unity's Android player enter JNI_OnLoad.
    try {
      const jniReport: string = probe.jni();
      hilog.info(0x0000, 'TransProbe', 'JNI_REPORT_BEGIN');
      for (const line of jniReport.split('\n')) {
        if (line.length > 0) {
          hilog.info(0x0000, 'TransProbe', '%{public}s', line);
        }
      }
      hilog.info(0x0000, 'TransProbe', 'JNI_REPORT_END');
      AppStorage.setOrCreate<string>('jniReport', jniReport);
    } catch (error) {
      hilog.error(0x0000, 'TransProbe', 'JNI_THREW %{public}s', JSON.stringify(error));
    }

    // Where does the installed HAP expose its rawfile tree?
    try {
      const pathReport: string = probe.paths();
      hilog.info(0x0000, 'TransProbe', 'PATH_REPORT_BEGIN');
      for (const line of pathReport.split('\n')) {
        if (line.length > 0) {
          hilog.info(0x0000, 'TransProbe', '%{public}s', line);
        }
      }
      hilog.info(0x0000, 'TransProbe', 'PATH_REPORT_END');
    } catch (error) {
      hilog.error(0x0000, 'TransProbe', 'PATH_THREW %{public}s', JSON.stringify(error));
    }

    // If the previous run crashed inside the engine, its own tombstone is readable here.
    try {
      const tombstone: string = probe.tombstone();
      hilog.info(0x0000, 'TransProbe', 'TOMBSTONE_BEGIN');
      for (const line of tombstone.split('\n')) {
        if (line.length > 0) {
          hilog.info(0x0000, 'TransProbe', '%{public}s', line);
        }
      }
      hilog.info(0x0000, 'TransProbe', 'TOMBSTONE_END');
    } catch (error) {
      hilog.error(0x0000, 'TransProbe', 'TOMBSTONE_THREW %{public}s', JSON.stringify(error));
    }

    // NOTE: the Unity bootstrap deliberately runs from ProbePage, after ArkUI has created the
    // surface. Starting it here would run the engine before an OHNativeWindow exists.
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    /*
     * Fullscreen plus hidden system bars. Applied again after the colour mode switch below: that
     * call is a configuration change, and the immersive state has to be re-asserted afterwards or
     * the status bar comes back and the surface is no longer edge to edge.
     *
     * Each step is guarded on its own: the first call in this lifecycle can fail with 1300002
     * (window state not ready yet), and an unguarded await would then skip everything after it -
     * including setWindowKeepScreenOn, which would let the screen sleep mid-song.
     */
    const enterImmersive = async (mainWindow: window.Window): Promise<void> => {
      try {
        await mainWindow.setWindowLayoutFullScreen(true);
      } catch (error) {
        hilog.warn(0x0000, 'TransProbe', 'FULLSCREEN_FAILED %{public}s', JSON.stringify(error));
      }
      try {
        await mainWindow.setWindowSystemBarEnable([]);
      } catch (error) {
        hilog.warn(0x0000, 'TransProbe', 'SYSTEMBAR_FAILED %{public}s', JSON.stringify(error));
      }
      try {
        await mainWindow.setWindowBackgroundColor('#000000');
      } catch (error) {
        hilog.warn(0x0000, 'TransProbe', 'BACKGROUND_FAILED %{public}s', JSON.stringify(error));
      }
    };

    const loadPage = (): void => {
      windowStage.loadContent(this.pageUri, (err) => {
        if (err.code) {
          hilog.error(0x0000, 'TransProbe', 'loadContent failed %{public}s', JSON.stringify(err));
          return;
        }
        /*
         * Registered dark for the whole application instead of following the device setting. The
         * API requires the window to exist and this page to be loaded, so it belongs here and not
         * in onCreate; everything drawn after this point (system dialogs, default component
         * colours) resolves dark.
         */
        try {
          this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
          hilog.info(0x0000, 'TransProbe', 'PROBE_COLOR_MODE dark');
        } catch (error) {
          hilog.warn(0x0000, 'TransProbe', 'COLOR_MODE_FAILED %{public}s', JSON.stringify(error));
        }
        windowStage.getMainWindow().then((mainWindow: window.Window) => enterImmersive(mainWindow))
          .catch((error: Error) => hilog.warn(0x0000, 'TransProbe', 'IMMERSIVE_REAPPLY failed %{public}s', JSON.stringify(error)));
      });
    };
    const keepAwake = async (mainWindow: window.Window): Promise<void> => {
      try {
        await mainWindow.setWindowKeepScreenOn(true);
        hilog.info(0x0000, 'TransProbe', 'PROBE_WINDOW_KEEP_AWAKE enabled');
      } catch (error) {
        hilog.warn(0x0000, 'TransProbe', 'KEEP_AWAKE_FAILED %{public}s', JSON.stringify(error));
      }
    };
    windowStage.getMainWindow().then(async (mainWindow: window.Window) => {
      await enterImmersive(mainWindow);
      await keepAwake(mainWindow);
      loadPage();
    }).catch((error: Error) => {
      hilog.warn(0x0000, 'TransProbe', 'WINDOW_SETUP failed %{public}s', JSON.stringify(error));
      loadPage();
    });
  }
}
