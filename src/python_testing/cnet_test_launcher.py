import subprocess
import os
import logging as logger


def run_cnet_tests(script_path, args):
    logger.info(f"Runnig {script_path}")
    subprocess.run(["python3", script_path] + args, check=True)


def main():
    test_scripts = [
        "src/python_testing/TC_CNET_1_4.py"
    ]

    args = [
        "--commissioning-method", "ble-wifi",
        "--discriminator", "3840",
        "--passcode", "20202021",
        "--wifi-ssid", os.environ.get("WIFI_SSID", "TestSSID"),
        "--wifi-passphrase", os.environ.get("WIFI_PASSPHRASE", "TestPassword")
    ]

    for test in test_scripts:
        run_cnet_tests(test, args)


if __name__ == "__main__":
    main()
