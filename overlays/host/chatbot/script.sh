#!/bin/sh
set -eu

CLEANUP="${CLEANUP:-1}"   # set to 0 to keep build tools
PIP_FLAGS="${PIP_FLAGS:---break-system-packages}"

echo "[*] Updating apt indices"
apt-get update

echo "[*] Installing Python + build deps (for llama-cpp-python build)"
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  python3 python3-pip python3-dev \
  build-essential \
  cmake ninja-build pkg-config \
  git ca-certificates

echo "[*] Upgrading pip tooling"
python3 -m pip install ${PIP_FLAGS} --upgrade pip setuptools wheel

echo "[*] Installing python packages (forcing native compiler)"
export CC=gcc
export CXX=g++

python3 -m pip install ${PIP_FLAGS} -r /dev/stdin <<'REQS'
langchain>=0.1.0,<0.3
langchain-community>=0.0.20
llama-cpp-python>=0.2.0
REQS

echo "[*] Python packages installed."

if [ "${CLEANUP}" = "1" ]; then
  echo "[*] Cleaning up build deps and caches"
  apt-get purge -y \
    python3-pip python3-dev \
    build-essential cmake ninja-build pkg-config git || true
  apt-get autoremove -y --purge || true
  rm -rf /root/.cache/pip /var/lib/apt/lists/* /var/cache/apt/archives/* || true
fi

echo "[*] Finished."
