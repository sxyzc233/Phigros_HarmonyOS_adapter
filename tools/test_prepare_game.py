import io
import unittest
import warnings
import zipfile
import prepare_game as prepare

class ImportTests(unittest.TestCase):
    def archive(self, names):
        stream=io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore',UserWarning)
            with zipfile.ZipFile(stream,'w') as z:
                for name in names:z.writestr(name,b'data')
        stream.seek(0)
        return zipfile.ZipFile(stream)
    def test_audio_change_preserves_elf_offsets(self):
        original=b'prefix\0libOpenSLES.so\0suffix'
        result=prepare.patch_audio(original)
        self.assertEqual(len(original),len(result))
        self.assertEqual(result,b'prefix\0libReaSLES.so\0\0suffix')
    def test_audio_rejects_unexpected_layout(self):
        for data in [b'wrong',b'libOpenSLES.so\0'*2]:
            with self.assertRaises(ValueError):prepare.patch_audio(data)
    def test_duplicate_members_rejected(self):
        with self.archive(['a','a']) as z:
            with self.assertRaisesRegex(ValueError,'duplicate'):prepare.checked_archive(z,[])
    def test_wrong_player_rejected(self):
        with self.archive(list(prepare.FINGERPRINTS)) as z:
            with self.assertRaisesRegex(ValueError,'Unsupported APK'):prepare.checked_archive(z,[])
    def test_unsafe_manifest_path_rejected(self):
        original=prepare.FINGERPRINTS
        try:
            prepare.FINGERPRINTS={}
            with self.archive([]) as z:
                for path in ['../outside','assets/bin/Data/../../outside','/absolute','assets/bin/Data/..\\outside']:
                    with self.assertRaisesRegex(ValueError,'Unsafe'):prepare.checked_archive(z,[path])
        finally:prepare.FINGERPRINTS=original

if __name__=='__main__':unittest.main()
