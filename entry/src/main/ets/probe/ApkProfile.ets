export interface ApkFingerprint { name: string; size: number; crc: number; }
export const APK_PROFILE_ID: string = '4.0.1.2';
export const APK_ASSET_COUNT: number = 3457;
export const APK_ASSET_INDEX: string = '983DCB24B0A9511534F065BAE05D4CE966155892DA6E4961F934DFBD643EF4EA';
export const APK_FINGERPRINTS: ApkFingerprint[] = [
  { name: 'lib/arm64-v8a/libunity.so', size: 20253848, crc: 0x1885092e },
  { name: 'lib/arm64-v8a/libil2cpp.so', size: 71737064, crc: 0xb933272b },
  { name: 'lib/arm64-v8a/libmain.so', size: 6728, crc: 0x581468ea },
  { name: 'lib/arm64-v8a/libnativeaudioe7.so', size: 108544, crc: 0xcdc4fc99 },
  { name: 'assets/bin/Data/Managed/Metadata/global-metadata.dat', size: 11287864, crc: 0x252444cb },
  { name: 'assets/bin/Data/boot.config', size: 151, crc: 0x65789d08 }
];
