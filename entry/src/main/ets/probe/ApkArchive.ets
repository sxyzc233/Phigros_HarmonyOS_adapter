import { fileIo, hash } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { APK_PROFILE_ID, APK_FINGERPRINTS, APK_ASSET_COUNT, APK_ASSET_INDEX } from './ApkProfile';

// Match uncompressed player/data identities, independent of APK compression.
export async function checkApkArchive(fd: number, size: number,
  readAt: (position: number, length: number) => Promise<ArrayBuffer>): Promise<void> {
  if (size < 22 || !Number.isSafeInteger(size)) throw new Error('所选文件不是完整的 Android APK。');
  const tailSize: number = Math.min(size, 65557);
  const tail = new DataView(await readAt(size - tailSize, tailSize));
  let end: number = -1;
  for (let i: number = tail.byteLength - 22; i >= 0; i--) {
    if (tail.getUint32(i, true) === 0x06054b50 && i + 22 + tail.getUint16(i + 20, true) === tail.byteLength) { end = i; break; }
  }
  if (end < 0) throw new Error('APK目录不完整，请重新下载文件后再导入。');
  const count: number = tail.getUint16(end + 10, true);
  const bytes: number = tail.getUint32(end + 12, true);
  const offset: number = tail.getUint32(end + 16, true);
  if (tail.getUint16(end + 4, true) !== 0 || tail.getUint16(end + 6, true) !== 0 ||
    count === 65535 || offset === 0xffffffff || bytes === 0xffffffff || bytes > 32 * 1024 * 1024 ||
    offset + bytes > size - tailSize + end) throw new Error('不支持此APK的压缩格式，请选择普通APK文件。');
  const buffer: ArrayBuffer = await readAt(offset, bytes);
  const directory = new DataView(buffer);
  const decoder = new util.TextDecoder();
  const found: Set<string> = new Set<string>();
  const assets: string[] = [];
  const names: Set<string> = new Set<string>();
  let cursor: number = 0;
  for (let i: number = 0; i < count; i++) {
    if (cursor + 46 > bytes || directory.getUint32(cursor, true) !== 0x02014b50) throw new Error('APK目录损坏，请重新下载后再试。');
    const nameLength: number = directory.getUint16(cursor + 28, true);
    const next: number = cursor + 46 + nameLength + directory.getUint16(cursor + 30, true) + directory.getUint16(cursor + 32, true);
    if (next > bytes) throw new Error('APK目录损坏，请重新下载后再试。');
    const name: string = decoder.decodeToString(new Uint8Array(buffer, cursor + 46, nameLength));
    if (names.has(name)) throw new Error('APK包含重复的文件目录，请选择完整原包。');
    names.add(name);
    if (name.startsWith('assets/') && !name.endsWith('/')) {
      assets.push(name + '\t' + directory.getUint32(cursor + 24, true).toString() + '\t' +
        directory.getUint32(cursor + 16, true).toString(16).padStart(8, '0') + '\n');
    }
    for (const expected of APK_FINGERPRINTS) {
      if (name !== expected.name) continue;
      if (found.has(name) || directory.getUint32(cursor + 24, true) !== expected.size ||
        directory.getUint32(cursor + 16, true) !== expected.crc || (directory.getUint16(cursor + 8, true) & 1) !== 0) {
        throw new Error('所选APK与当前适配版本不匹配。本版需要 Phigros ' + APK_PROFILE_ID + '；请重新选择对应原包。');
      }
      found.add(name);
    }
    cursor = next;
  }
  if (found.size !== APK_FINGERPRINTS.length) throw new Error('所选APK缺少本版所需资源。本版需要 Phigros ' + APK_PROFILE_ID + ' 的完整 Android 原包。');
  const index = hash.createHash('sha256');
  index.update(new util.TextEncoder().encodeInto(assets.sort().join('')).buffer as ArrayBuffer);
  if (assets.length !== APK_ASSET_COUNT || index.digest().toUpperCase() !== APK_ASSET_INDEX) {
    throw new Error('APK游戏资源与本版不匹配或不完整。本版需要 Phigros ' + APK_PROFILE_ID + ' 的完整 Android 原包。');
  }
  fileIo.lseek(fd, 0, fileIo.WhenceType.SEEK_SET);
}
