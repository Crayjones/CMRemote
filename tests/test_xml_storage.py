import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class XmlStorageContract(unittest.TestCase):
    def test_xmlOnlyStorageContractV73(self):
        retired = ('app/CredentialVault.cpp', 'app/CredentialVault.h',
                   'app/VaultService.cpp', 'app/VaultService.h',
                   'app/LegacyConnection.cpp', 'app/LegacyConnection.h',
                   'app/MainWindowVault.cpp', 'app/VaultDialogs.cpp', 'app/VaultDialogs.h',
                   'app/ui/SavedCredentialsDialog.ui', 'app/ui/VaultPassphraseDialog.ui',
                   'core/src/persistence.rs', 'tests/credential_vault.cpp',
                   'tests/vault_service.cpp', 'tests/vault_shell.cpp', 'tests/legacy_conversion.cpp')
        self.assertEqual([path for path in retired if (ROOT / path).exists()], [])
        sources = '\n'.join(path.read_text() for path in (ROOT / 'app').glob('*.[ch]*'))
        self.assertNotRegex(sources, r'CredentialVault|VaultService|LegacyConnection|convertLegacy|xmlMode_')
        self.assertNotRegex(sources, r'profiles\.json|RMCONS|RmLegacyCredentials')
        core = '\n'.join(path.read_text() for path in (ROOT / 'core/src').glob('*.rs'))
        self.assertNotRegex(core, r'open_core|pub fn open\(|pub mod persistence|std::fs|PathBuf')
        self.assertNotRegex(core, r'pub fn (?:add|create|update|delete)_profile\(')
        build = (ROOT / 'CMakeLists.txt').read_text()
        self.assertNotRegex(build, r'vault_(?:shell|service)_test|credential_vault_test|legacy_conversion_test')
        self.assertIn('xml_storage_test', build)
        paths = (ROOT / 'app/DataPaths.h').read_text()
        self.assertIn('filePath("confCons.xml")', paths)
        selection = paths.split('applicationConnectionPath', 1)[1].split('}', 1)[0]
        self.assertNotIn('exists(', selection)
        form = (ROOT / 'app/ui/CredentialsDialog.ui').read_text()
        self.assertNotIn('rememberPasswordCheck', form)
        # Dependency/tool manifests and bounded password-free bridge projections
        # are not connection-file storage; serde_json remains intentional.
        projection = (ROOT / 'core/src/projection.rs').read_text()
        self.assertIn('16 * 1024 * 1024', projection)
        self.assertIn('serde_json::from_slice', projection)
        self.assertIn('serde_json::to_string', projection)


if __name__ == '__main__':
    unittest.main()
