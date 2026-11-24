"""Host-only family compression profile policy: opt-in, validated, baseline retained."""
from pathlib import Path
import importlib.util
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('family_game', ROOT / 'dm/games/game.py')
game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(game)


class FamilyCompressionProfile(unittest.TestCase):
    def header(self, value):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / '0100000000F0F000'
            folder.mkdir()
            rows = ('title_id\t0100000000F0F000\nname\tFixture\nprogram_id\t0100000000F0F000\nmodules\tmain\n'
                    'romfs_marker\tromfs.img\nmem_reserve\t0x100000\nmem_alias\t0x100000\nmem_stack\t0x100000\napp_pool\t0x100000\n')
            if value is not None:
                rows += 'family_compress\t' + value + '\n'
            (folder / 'game.tsv').write_text(rows)
            return game.header(game.Game(folder))

    def test_baseline_without_key(self):
        self.assertIn('#define DM_GAME_FAMILY_COMPRESS 0', self.header(None))

    def test_explicit_policies(self):
        for value, expected in (('off', 0), ('on', 1)):
            self.assertIn(f'#define DM_GAME_FAMILY_COMPRESS {expected}', self.header(value))

    def test_invalid_policy_fails(self):
        for value in ('1', 'auto', 'yes'):
            with self.assertRaisesRegex(SystemExit, 'family_compress must be on or off'):
                self.header(value)

    def test_ascension_retains_unqualified_baseline(self):
        self.assertEqual(game.Game(ROOT / 'dm/games/0400486024DEC000').get('family_compress'), 'off')


if __name__ == '__main__':
    unittest.main()
