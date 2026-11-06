import { fileIo } from '@kit.CoreFileKit';
import { hiAppEvent, hilog } from '@kit.PerformanceAnalysisKit';
import probe from 'libtransprobe.so';

/** App-owned reports only. No memory dumps, account files or credential logging. */
export class ReaCrashDiagnostics {
  private directory: string = '';
  private sequence: number = 0;
  private copied: Set<string> = new Set<string>();
  private walk(source: string, depth: number): void {
    if (depth > 5) return;
    try {
      for (const name of fileIo.listFileSync(source)) {
        const path: string = source + '/' + name;
        const stat = fileIo.lstatSync(path);
        if (stat.isSymbolicLink()) continue;
        if (stat.isDirectory()) this.walk(path, depth + 1);
        else if (stat.isFile() && (/\.(txt|log|json)$/i.test(name) || /^(cppcrash|jscrash|appfreeze|fault)/i.test(name)) && stat.size <= 4 * 1024 * 1024) this.copyReport(path);
      }
    } catch (_) { /* Some system event directories may not contain reports yet. */ }
  }
  private copyReport(source: string, depth: number = 0): void {
    if (depth > 2 || this.copied.has(source)) return;
    this.copied.add(source);
    try {
      const destination: string = this.directory + '/report-' + (++this.sequence) + '.txt';
      fileIo.copyFileSync(source, destination);
      const readable: boolean = probe.publishCrashReport(destination);
      // Export only stack attribution; never registers, memory, tokens or save data.
      const report: string = fileIo.readTextSync(destination);
      const decoded: string = report.replace(/\\n/g, '\n');
      const external: string[] = report.match(/\/data\/storage\/el2\/(?:log|base\/(?:haps\/entry\/)?files\/hiappevent)\/[A-Za-z0-9_./:-]+/g) ?? [];
      for (const path of external.slice(0, 12)) if (!path.includes('..')) this.copyReport(path, depth + 1);
      if (report.trim().startsWith('{')) {
        try {
          const keys: string[] = Object.keys(JSON.parse(report) as Record<string, Object>).filter((key: string) => /^[A-Za-z_]+$/.test(key));
          hilog.info(0, 'TransCrashStack', 'REPORT_KEYS %{public}s', keys.slice(0, 24).join(','));
        } catch (_) {}
      }
      const fieldNames: string[] = report.match(/["'][A-Za-z_]+["']\s*:/g) ?? [];
      hilog.info(0, 'TransCrashStack', 'REPORT_FIELDS %{public}s', fieldNames.slice(0, 60).join(','));
      for (const line of report.split('\n')) {
        try {
          const row = JSON.parse(line) as Record<string, Object>;
          hilog.info(0, 'TransCrashStack', 'REPORT_ROW_KEYS %{public}s', Object.keys(row).filter((key: string) => /^[A-Za-z_]+$/.test(key)).slice(0, 30).join(','));
        } catch (_) {}
      }
      const lines: string[] = decoded.split('\n');
      const matches: string[] = decoded.match(/#[0-9]{2}\s+pc\s+[^\n\\"]{1,220}/g) ?? [];
      for (const frame of matches.slice(0, 32)) hilog.error(0, 'TransCrashStack', '%{public}s', frame);
      const signals: string[] = decoded.match(/SIG(SEGV|ABRT|BUS|ILL|TRAP|FPE)[^\n\\"]{0,80}/g) ?? [];
      for (const signal of signals.slice(0, 2)) hilog.error(0, 'TransCrashStack', '%{public}s', signal);
      hilog.info(0, 'TransCrashStack', 'REPORT_SHAPE bytes=%{public}d lines=%{public}d frames=%{public}d json=%{public}d', report.length, lines.length, matches.length, report.trim().startsWith('{') ? 1 : 0);
      let frames: number = 0;
      for (const line of lines) {
        if (/^(Reason:|Pid:|Tid:|Fault thread info:|Thread name:|#\d{2}\s+pc)/.test(line.trim()) && frames++ < 48) {
          hilog.error(0, 'TransCrashStack', '%{public}s', line.substring(0, 900));
        }
      }
      hilog.info(0, 'TransCrash', 'CRASH_REPORT_EXPORTED index=%{public}d', this.sequence);
      hilog.info(0, 'TransCrash', 'CRASH_REPORT_READABLE result=%{public}d', readable ? 1 : 0);
    } catch (_) { hilog.warn(0, 'TransCrash', 'CRASH_REPORT_COPY_FAILED'); }
  }
  start(filesDir: string): void {
    this.directory = filesDir + '/rea-crash-diagnostics';
    try { fileIo.mkdirSync(this.directory, true); } catch (_) {}
    this.walk(filesDir + '/hiappevent', 0);
    this.walk('/data/storage/el2/base/files/hiappevent', 0);
    try {
      hiAppEvent.addWatcher({ name: 'reaNativeCrashes',
        appEventFilters: [{ domain: hiAppEvent.domain.OS, names: [hiAppEvent.event.APP_CRASH, hiAppEvent.event.APP_FREEZE] }],
        onReceive: (_: string, groups: hiAppEvent.AppEventGroup[]) => {
          for (const group of groups) for (const info of group.appEventInfos) {
            hilog.error(0, 'TransCrash', 'CRASH_EVENT_RECEIVED name=%{public}s', group.name);
            const params: Record<string, hiAppEvent.ParamType> = info.params as Record<string, hiAppEvent.ParamType>;
            const paths: hiAppEvent.ParamType = params['external_log'];
            if (typeof paths === 'string' && (paths.startsWith(filesDir + '/hiappevent/') || paths.startsWith('/data/storage/el2/log/'))) this.copyReport(paths);
            if (Array.isArray(paths)) for (const item of paths as string[]) {
              if (typeof item === 'string' && (item.startsWith(filesDir + '/hiappevent/') || item.startsWith('/data/storage/el2/log/'))) this.copyReport(item);
            }
          }
        }
      });
    } catch (_) { hilog.warn(0, 'TransCrash', 'CRASH_WATCHER_UNAVAILABLE'); }
  }
}
