#!/bin/sh
set -eu

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  python3 python3-venv python3-dev build-essential cmake ninja-build pkg-config

python3 -m venv /opt/agentee --system-site-packages

export CC=gcc CXX=g++
/opt/agentee/bin/pip install \
  'langchain>=0.3.0' \
  'langchain-community>=0.3.0' \
  'llama-cpp-python>=0.2.0'

echo 'PATH=/opt/agentee/bin:$PATH' > /etc/profile.d/agentee.sh
echo '. /opt/agentee/bin/activate' >> /home/user/.bashrc

echo "Done. Open a new shell or run: source /opt/agentee/bin/activate"
