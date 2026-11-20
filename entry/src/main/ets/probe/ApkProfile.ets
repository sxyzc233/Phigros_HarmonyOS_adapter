export interface ApkFingerprint { name: string; size: number; crc: number; }
export const APK_PROFILE_ID: string = '4.1.0';
export const APK_ASSET_COUNT: number = 3475;
export const APK_ASSET_INDEX: string = '0C60FED432FFF3BA38298A023ED92893200758B94196D7FE1CB72AE011DFB05B';
export const APK_FINGERPRINTS: ApkFingerprint[] = [
  { name: 'lib/arm64-v8a/libunity.so', size: 20253848, crc: 0x44322af8 },
  { name: 'lib/arm64-v8a/libil2cpp.so', size: 71736312, crc: 0xdf3281f9 },
  { name: 'lib/arm64-v8a/libmain.so', size: 6728, crc: 0x581468ea },
  { name: 'lib/arm64-v8a/libnativeaudioe7.so', size: 108544, crc: 0xcdc4fc99 },
  { name: 'assets/bin/Data/Managed/Metadata/global-metadata.dat', size: 11288008, crc: 0xa8094e98 },
  { name: 'assets/bin/Data/boot.config', size: 151, crc: 0xeeb70f80 }
];
