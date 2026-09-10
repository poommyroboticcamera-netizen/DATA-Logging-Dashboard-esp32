from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CAN_SOURCE = (ROOT / "src" / "can" / "CanService.cpp").read_text(encoding="utf-8")
MAIN_SOURCE = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


class PassiveContractTest(unittest.TestCase):
    def test_dashboard_firmware_has_no_can_transmit_call(self):
        production = CAN_SOURCE + "\n" + MAIN_SOURCE
        self.assertNotIn("twai_transmit", production)

    def test_driver_is_hard_configured_listen_only(self):
        self.assertIn("TWAI_MODE_LISTEN_ONLY", CAN_SOURCE)
        self.assertRegex(CAN_SOURCE, re.compile(r"general\.tx_queue_len\s*=\s*0\s*;"))
        self.assertNotIn("TWAI_MODE_NORMAL", CAN_SOURCE)
        self.assertNotIn("TWAI_MODE_NO_ACK", CAN_SOURCE)


if __name__ == "__main__":
    unittest.main()
