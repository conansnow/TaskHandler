#!/usr/bin/env bash
# Install LLVM 23 packages from apt.llvm.org. The installer script is
# checksummed because it runs as root; bump the hash when updating the pin.
set -euo pipefail

# sha256 of https://apt.llvm.org/llvm.sh as of 2026-09-18
expected='03878e08f47b66cc95bc4b544b0db3c6d9ce8d60e6cf2492ae357984330a9eae'

sudo apt-get update
sudo apt-get install -y wget lsb-release software-properties-common gnupg
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
echo "${expected}  /tmp/llvm.sh" | sha256sum -c -
sudo bash /tmp/llvm.sh 23
sudo apt-get install -y "$@"
