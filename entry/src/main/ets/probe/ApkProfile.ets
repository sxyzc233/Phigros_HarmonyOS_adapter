export interface ApkFingerprint { name: string; size: number; crc: number; }
export const APK_PROFILE_ID: string = '4.1.0.1';
export const APK_DOWNLOAD_BYTES: number = 2483459032;
export const APK_DOWNLOAD_SHA256: string = 'aacfc462fb3ba77d937571f2f72e3b5a549e08d25fbae3f593890bad7791b13b';
export const APK_ASSET_COUNT: number = 3475;
export const APK_ASSET_INDEX: string = '7F9F29E0270BA40534711005AD79D692422D4786D8E8222D0BDDBA30C9BAD719';
export const APK_FINGERPRINTS: ApkFingerprint[] = [
  { name: 'lib/arm64-v8a/libunity.so', size: 20253848, crc: 0x44322af8 },
  { name: 'lib/arm64-v8a/libil2cpp.so', size: 71736312, crc: 0xe2ddc6f0 },
  { name: 'lib/arm64-v8a/libmain.so', size: 6728, crc: 0x581468ea },
  { name: 'lib/arm64-v8a/libnativeaudioe7.so', size: 108544, crc: 0xcdc4fc99 },
  { name: 'assets/bin/Data/Managed/Metadata/global-metadata.dat', size: 11288016, crc: 0xdd7ac4f4 },
  { name: 'assets/bin/Data/boot.config', size: 151, crc: 0x822e1f73 }
];
