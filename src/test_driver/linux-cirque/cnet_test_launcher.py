import logging
import os
import subprocess
import sys

from helper.CHIPTestBase import CHIPVirtualHome

logger = logging.getLogger("CNETTestLauncher")
logging.basicConfig(level=logging.INFO)


class CNETTests(CHIPVirtualHome):
    def __init__(self, commissioning_method):
        super().__init__("http://localhost:5000", device_config={
            "device0": {
                "type": "CHIP-DUT",
                "base_image": "@default",
                "compatibility": {"Interactive": True}
            },
            "device1": {
                "type": "CHIP-TH",
                "base_image": "@default",
                "compatibility": {"Interactive": True}
            }
        })
        self.commissioning_method = commissioning_method

    def setup(self):
        self.initialize_home()

        if self.commissioning_method == "on-network":
            logger.info("Connecting DUT to Wi-Fi before running tests")
            self.connect_to_wifi_network()

    def run_cnet_tests(self, script_path, args):
        logger.info(f"Running {script_path} with args: {args}")
        subprocess.run(["python3", script_path] + args, check=True)


def main():
    commissioning_method = "ble-wifi"
    test_scripts = [
        "src/python_testing/TC_CNET_1_4.py"
    ]

    args = [
        "--commissioning-method", commissioning_method,
        "--discriminator", "3840",
        "--passcode", "20202021",
        "--wifi-ssid", os.environ.get("WIFI_SSID", "TestSSID"),
        "--wifi-passphrase", os.environ.get("WIFI_PASSPHRASE", "TestPassword")
    ]

    try:
        cnet = CNETTests(commissioning_method)
        cnet.setup()

        for test in test_scripts:
            cnet.run_cnet_tests(test, args)

        sys.exit(0)

    except subprocess.CalledProcessError as e:
        logger.error(f"Test script failed: {e}")
        sys.exit(1)

    except Exception as ex:
        logger.exception("Unexpected failure occured.")
        sys.exit(2)


if __name__ == "__main__":
    main()
