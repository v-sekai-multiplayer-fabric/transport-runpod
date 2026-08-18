"""The worker. RunPod's SDK runs the job loop; the handler asks the bus.

SPDX-License-Identifier: Apache-2.0
"""

import runpod

from .handler import Handler


def main() -> int:
    runpod.serverless.start({"handler": Handler()})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
