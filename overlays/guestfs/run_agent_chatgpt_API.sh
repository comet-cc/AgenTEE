#!/bin/bash
nohup redis-server --bind 127.0.0.1 --port 6379 > /tmp/redis.log 2>&1 &
export GMAIL_AUTH_MODE=manual_code
export OPENAI_API_KEY="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"
cd "$( dirname "${BASH_SOURCE[0]}" )"/SecGPT

#python3 secgpt_main.py "$@" --query "Book me a healthcare appointment" --model-provider chatgpt --openai-model gpt-4.1-mini
python3 secgpt_main.py --model-provider chatgpt --openai-model gpt-4.1-mini
