from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CodexBuddyBoardingPassTest(unittest.TestCase):
    def test_boarding_pass_page_contract(self):
        app = (ROOT / "main/apps/app_codex_buddy/app_codex_buddy.cpp").read_text()
        sdkconfig = (ROOT / "sdkconfig.defaults").read_text()

        self.assertIn("CONFIG_LV_USE_QRCODE=y", sdkconfig)
        self.assertIn("static constexpr uint8_t kPageCount = 5;", app)
        self.assertIn("Boarding Pass", app)
        self.assertIn("lv_qrcode_create", app)
        self.assertIn("lv_qrcode_update", app)


if __name__ == "__main__":
    unittest.main()
