import { fileIo, hash, statfs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { request } from '@kit.BasicServicesKit';
import { url } from '@kit.ArkTS';
import { APK_DOWNLOAD_BYTES, APK_DOWNLOAD_SHA256, APK_PROFILE_ID } from './ApkProfile';
import { checkApkArchive } from './ApkArchive';

export const ORIGINAL_APK_PATH: string = '/data/storage/el2/base/files/rea-original-phigros.apk';
const MARKER_PATH: string = ORIGINAL_APK_PATH + '.verified.json';
const CHUNK_SIZE: number = 2 * 1024 * 1024;
let importSerial: number = 0;
interface ImportedApk { profile: string; bytes: number; sha256: string; }

class ImportFiles {
  source: fileIo.File | undefined = undefined;
  destination: fileIo.File | undefined = undefined;
  temporary: string = '';
  delayedCleanup: boolean = false;

  async cleanup(): Promise<void> {
    if (this.source !== undefined) { await fileIo.close(this.source).catch(() => {}); this.source = undefined; }
    if (this.destination !== undefined) { await fileIo.close(this.destination).catch(() => {}); this.destination = undefined; }
    if (this.temporary.length > 0) {
      try { await fileIo.unlink(this.temporary); } catch (_) { }
      try { await fileIo.unlink(this.temporary + '.json'); } catch (_) { }
    }
  }

  async wait<T>(operation: Promise<T>, phase: string): Promise<T> {
    let timer: number = -1;
    const timeout = new Promise<T>((_resolve, reject) => {
      timer = setTimeout(() => {
        this.delayedCleanup = true;
        // Never close/reuse descriptors while an outstanding IO still uses them.
        operation.then(() => this.cleanup(), () => this.cleanup());
        reject(new Error(phase + '超时。请先把APK保存到“下载”文件夹，再从本机选择后重试。'));
      }, 30000);
    });
    try { return await Promise.race<T>([operation, timeout]); }
    finally { if (timer >= 0) clearTimeout(timer); }
  }

  async readAt(position: number, length: number): Promise<ArrayBuffer> {
    if (this.source === undefined) throw new Error('无法读取所选文件，请重新选择。');
    fileIo.lseek(this.source.fd, position, fileIo.WhenceType.SEEK_SET);
    const bytes = new ArrayBuffer(length);
    let done: number = 0;
    while (done < length) {
      fileIo.lseek(this.source.fd, position + done, fileIo.WhenceType.SEEK_SET);
      const part = new ArrayBuffer(length - done);
      const count: number = await this.wait(fileIo.read(this.source.fd, part), '读取文件');
      if (count <= 0) throw new Error('文件读取不完整，请重新下载APK。');
      new Uint8Array(bytes).set(new Uint8Array(part, 0, count), done); done += count;
    }
    return bytes;
  }
}

export function originalApkReady(): boolean {
  try {
    const marker: ImportedApk = JSON.parse(fileIo.readTextSync(MARKER_PATH)) as ImportedApk;
    const info = fileIo.statSync(ORIGINAL_APK_PATH);
    return marker.profile === APK_PROFILE_ID && marker.sha256.length === 64 && info.isFile() && info.size === marker.bytes;
  } catch (_) { return false; }
}

export async function existingApkCompatible(): Promise<boolean> {
  if (originalApkReady()) return true;
  const files = new ImportFiles();
  try {
    files.source = await files.wait(fileIo.open(ORIGINAL_APK_PATH, fileIo.OpenMode.READ_ONLY).then(file => {
      files.source = file; return file;
    }), '打开已有资源');
    const info = await files.wait(fileIo.stat(files.source!.fd), '检查已有资源');
    await checkApkArchive(files.source!.fd, info.size, (offset: number, length: number) => files.readAt(offset, length));
    return true;
  } catch (_) { return false; }
  finally { if (!files.delayedCleanup) await files.cleanup(); }
}

async function saveVerifiedApk(temporary: string, marker: ImportedApk): Promise<void> {
  const markerTemp: string = temporary + '.json';
  const record = await fileIo.open(markerTemp, fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY);
  try { await fileIo.write(record.fd, JSON.stringify(marker)); await fileIo.fsync(record.fd); }
  finally { await fileIo.close(record); }

  let backup: string = '';
  if (await fileIo.access(ORIGINAL_APK_PATH)) {
    backup = ORIGINAL_APK_PATH + '.previous.' + Date.now().toString();
    await fileIo.rename(ORIGINAL_APK_PATH, backup);
  }
  let installed: boolean = false;
  try {
    await fileIo.rename(temporary, ORIGINAL_APK_PATH);
    installed = true;
    await fileIo.rename(markerTemp, MARKER_PATH);
  } catch (error) {
    if (installed) await fileIo.rename(ORIGINAL_APK_PATH, temporary);
    if (backup.length > 0) await fileIo.rename(backup, ORIGINAL_APK_PATH);
    throw error as Error;
  }
}

export async function downloadOriginalApk(context: common.UIAbilityContext, address: string,
  onStatus: (message: string) => void): Promise<void> {
  const link: url.URL = url.URL.parseURL(address.trim());
  if (link.protocol !== 'https:' || link.username.length > 0 || link.password.length > 0) {
    throw new Error('请输入 HTTPS APK 直链。');
  }
  const files = new ImportFiles();
  const temporary: string = ORIGINAL_APK_PATH + '.download';
  files.temporary = temporary;
  try {
    await fileIo.unlink(temporary).catch(() => {});
    const free: number = await statfs.getFreeSize(context.filesDir);
    if (free < APK_DOWNLOAD_BYTES + 500 * 1024 * 1024) {
      throw new Error('请预留约 3 GB 空间用于下载和准备游戏资源。');
    }
    onStatus('正在连接下载地址……');
    const options: request.agent.Config = {
      action: request.agent.Action.DOWNLOAD,
      url: link.toString(),
      mode: request.agent.Mode.FOREGROUND,
      title: 'Phigros ' + APK_PROFILE_ID,
      saveas: 'file://' + context.abilityInfo.bundleName + temporary,
      overwrite: false
    };
    const task: request.agent.Task = await request.agent.create(context, options);
    try {
      await new Promise<void>((resolve, reject) => {
        task.on('progress', (progress: request.agent.Progress) => {
          const total: number = progress.sizes.length > 0 ? progress.sizes[0] : -1;
          if ((total > 0 && total !== APK_DOWNLOAD_BYTES) || progress.processed > APK_DOWNLOAD_BYTES) {
            reject(new Error('下载内容的大小与当前版本原包不符。'));
            return;
          }
          onStatus('正在下载 ' + Math.floor(100 * progress.processed / APK_DOWNLOAD_BYTES).toString() + '%');
        });
        task.on('completed', () => { resolve(); });
        task.on('failed', () => { reject(new Error('APK 下载失败，请检查网络和直链。')); });
        task.on('pause', () => { reject(new Error('下载已暂停，请在网络恢复后重试。')); });
        task.on('remove', () => { reject(new Error('下载已结束，请重试。')); });
        task.start().catch(() => { reject(new Error('无法开始下载，请检查直链或剩余空间。')); });
      });

      onStatus('正在校验下载的 APK……');
      files.source = await fileIo.open(temporary, fileIo.OpenMode.READ_ONLY);
      const info = await fileIo.stat(files.source.fd);
      if (info.size !== APK_DOWNLOAD_BYTES) throw new Error('下载的 APK 不完整或版本不匹配。');
      await checkApkArchive(files.source.fd, info.size, (offset: number, length: number) => files.readAt(offset, length));
      fileIo.lseek(files.source.fd, 0, fileIo.WhenceType.SEEK_SET);
      const digest = hash.createHash('sha256');
      const buffer = new ArrayBuffer(CHUNK_SIZE);
      let remaining: number = info.size;
      while (remaining > 0) {
        const count: number = await files.wait(fileIo.read(files.source.fd, buffer,
          { length: Math.min(CHUNK_SIZE, remaining) }), '校验资源');
        if (count <= 0) throw new Error('下载的 APK 读取不完整。');
        digest.update(count === buffer.byteLength ? buffer : buffer.slice(0, count));
        remaining -= count;
      }
      await fileIo.close(files.source);
      files.source = undefined;
      const sha256: string = digest.digest().toLowerCase();
      if (sha256 !== APK_DOWNLOAD_SHA256) throw new Error('APK 的 SHA-256 与当前版本原包不匹配。');
      await saveVerifiedApk(temporary, { profile: APK_PROFILE_ID, bytes: info.size, sha256: sha256 });
    } finally {
      await request.agent.remove(task.tid).catch(() => {});
    }
  } finally {
    if (!files.delayedCleanup) await files.cleanup();
  }
}

export async function importOriginalApk(uri: string, onStatus: (message: string) => void): Promise<void> {
  const files = new ImportFiles();
  files.temporary = ORIGINAL_APK_PATH + '.importing.' + Date.now().toString() + '.' + (++importSerial).toString();
  let phase: string = '打开文件';
  const began: number = Date.now();
  try {
    onStatus('正在打开所选APK……');
    files.source = await files.wait(fileIo.open(uri, fileIo.OpenMode.READ_ONLY).then(file => {
      files.source = file; return file;
    }), phase);
    const info = await files.wait(fileIo.stat(files.source!.fd), '检查文件');
    onStatus('正在识别APK内容……'); phase = '识别版本';
    await checkApkArchive(files.source!.fd, info.size, (offset: number, length: number) => files.readAt(offset, length));
    const free: number = await files.wait(statfs.getFreeSize('/data/storage/el2/base/files'), '检查存储空间');
    if (free < info.size + 64 * 1024 * 1024) throw new Error('可用空间不足，导入还需要约 ' + (info.size / 1000000000).toFixed(2) + ' GB。');
    files.destination = await files.wait(fileIo.open(files.temporary,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE).then(file => {
      files.destination = file; return file;
    }), '创建资源文件');
    const digest = hash.createHash('sha256');
    const buffer = new ArrayBuffer(CHUNK_SIZE);
    let done: number = 0; let lastShown: number = 0; let lastLogged: number = -1;
    phase = '复制资源';
    hilog.info(0, 'TransImport', 'IMPORT_BEGIN profile=%{public}s bytes=%{public}d', APK_PROFILE_ID, info.size);
    while (done < info.size) {
      fileIo.lseek(files.source!.fd, done, fileIo.WhenceType.SEEK_SET);
      const count: number = await files.wait(fileIo.read(files.source!.fd, buffer,
        { length: Math.min(CHUNK_SIZE, info.size - done) }), '读取资源');
      if (count <= 0) throw new Error('APK读取提前结束，请重新下载完整文件。');
      const chunk: ArrayBuffer = count === buffer.byteLength ? buffer : buffer.slice(0, count);
      digest.update(chunk);
      let written: number = 0;
      while (written < count) {
        fileIo.lseek(files.destination!.fd, done + written, fileIo.WhenceType.SEEK_SET);
        const part: ArrayBuffer = written === 0 ? chunk : chunk.slice(written);
        const n: number = await files.wait(fileIo.write(files.destination!.fd, part), '写入资源');
        if (n <= 0) throw new Error('资源写入失败，请检查剩余空间。');
        written += n;
      }
      done += count;
      const percent: number = Math.floor(done / info.size * 100);
      if (Date.now() - lastShown >= 150 || done === info.size) {
        onStatus('正在导入 ' + percent.toString() + '%（' + (done / 1000000).toFixed(0) + ' / ' +
          (info.size / 1000000).toFixed(0) + ' MB）'); lastShown = Date.now();
      }
      if (Math.floor(percent / 10) > lastLogged) {
        lastLogged = Math.floor(percent / 10);
        hilog.info(0, 'TransImport', 'IMPORT_PROGRESS percent=%{public}d bytes=%{public}d', percent, done);
      }
    }
    onStatus('正在保存资源……'); phase = '保存资源';
    await files.wait(fileIo.fsync(files.destination!.fd), phase);
    await fileIo.close(files.destination); files.destination = undefined;
    await fileIo.close(files.source); files.source = undefined;
    const marker: ImportedApk = { profile: APK_PROFILE_ID, bytes: info.size, sha256: digest.digest() };
    await saveVerifiedApk(files.temporary, marker);
    hilog.info(0, 'TransImport', 'IMPORT_DONE profile=%{public}s bytes=%{public}d time_ms=%{public}d', APK_PROFILE_ID, info.size, Date.now() - began);
  } catch (error) {
    hilog.error(0, 'TransImport', 'IMPORT_FAILED phase=%{public}s', phase);
    throw error as Error;
  } finally { if (!files.delayedCleanup) await files.cleanup(); }
}

