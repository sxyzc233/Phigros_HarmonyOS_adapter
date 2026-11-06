import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

// Private rollback copy before this compatibility fix touches game startup.
// Never export or log its contents; it can include a saved login session.
export function backupBeforeSyncFix(): void {
  const root: string = '/data/storage/el2/base/files';
  const destination: string = root + '/rea-sync-recovery/036';
  try {
    if (fileIo.accessSync(destination + '/complete')) return;
    fileIo.mkdirSync(destination, true);
    let copied: number = 0;
    if (fileIo.accessSync(root + '/rea-jprefs')) {
      fileIo.mkdirSync(destination + '/prefs', true);
      for (const name of fileIo.listFileSync(root + '/rea-jprefs')) {
        if (!/^[0-9a-f]{16}\.prefs(\.bak)?$/.test(name)) continue;
        fileIo.copyFileSync(root + '/rea-jprefs/' + name, destination + '/prefs/' + name); copied++;
      }
    }
    if (fileIo.accessSync(root + '/.userdata')) {
      fileIo.copyFileSync(root + '/.userdata', destination + '/userdata-private'); copied++;
    }
    const marker = fileIo.openSync(destination + '/complete', fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
    try { fileIo.writeSync(marker.fd, '036\n'); fileIo.fsyncSync(marker.fd); } finally { fileIo.closeSync(marker); }
    hilog.info(0, 'TransRecovery', 'PRE_SYNC_FIX_BACKUP complete=1 files=%{public}d (private contents withheld)', copied);
  } catch (_) { hilog.error(0, 'TransRecovery', 'PRE_SYNC_FIX_BACKUP complete=0 (no files removed)'); }
}
