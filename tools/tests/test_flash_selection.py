"""Board selection regression tests; no esptool or attached device needed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('flash_poc', Path(__file__).resolve().parents[1] / 'flash_poc.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class ImageSelection(unittest.TestCase):
    def test_each_board_alone_and_explicit(self):
        for board in flash.BOARDS:
            bdir = 'build' if board == 'c606' else 'build-' + board
            with self.subTest(board=board), patch.object(flash.os.path, 'exists',
                    side_effect=lambda p: str(p).endswith(f'{bdir}/{board}_oss.bin')):
                self.assertTrue(flash.default_app().endswith(f'{board}_oss.bin'))
                self.assertTrue(flash.default_app(board).endswith(f'{board}_oss.bin'))

    def test_multiple_boards_require_selection(self):
        with patch.object(flash.os.path, 'exists', return_value=True):
            with self.assertRaisesRegex(ValueError, 'multiple boards'):
                flash.default_app()
            for board in flash.BOARDS:
                self.assertTrue(flash.default_app(board).endswith(f'{board}_oss.bin'))

    def test_single_release_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            for board in flash.BOARDS:
                image = Path(tmp) / f'{board}_oss.bin'
                image.touch()
                with patch.object(flash, '__file__', str(Path(tmp) / 'flash_poc.py')):
                    self.assertEqual(Path(flash.default_app(board)), image)
                image.unlink()

    def test_ambiguity_fails_before_serial_access(self):
        with patch.object(flash.sys, 'argv', ['flash_poc.py', '-p', 'unused']), \
                patch.object(flash.os.path, 'exists', return_value=True), \
                patch.object(flash, 'read_flash') as read:
            with self.assertRaises(SystemExit) as err:
                flash.main()
            self.assertEqual(err.exception.code, 2)
            read.assert_not_called()


if __name__ == '__main__':
    unittest.main()
