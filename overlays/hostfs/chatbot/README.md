

# Conversational Agent
This repository demonstrates a simple inter-process communication (IPC) setup where a **client (agent)** and a **local LLM worker** communicate through a shared device file. Both code must see the same device, which is either the CSM or an arbitrary created device.

## Overview

- **Model**
  - Loads a local GGUF LLM using `llama.cpp`
  - Waits for prompts from a shared device
  - Runs inference
  - Sends responses back through the same device
  - Worked well with **GPT2-Large** 

- **Agent**
  - Reads user input from stdin
  - Sends a prompt to the model
  - Waits for the response
  - Prints the model output

## Files
- `model.py`  
  Runs the LLM and serves incoming requests.

- `agent.py`  
  Sends a prompt and prints the model response.
  
- `CSM.py`
  Functions to communicate over the shared device

## Shared Device
Communication happens through a shared device file.

- Default path: `/tmp/shmfile`
- Can be overridden using the `--device` argument

If the device file does not exist, it is automatically created for testing purposes.

## Usage
Run the model process first:

```
python3 model.py \
  --model ../models/gpt2-large-q8_0.gguf \
  --device /tmp/shmfile
```
Run the agent in another console:
```
python agent.py \
  --device /tmp/shmfile
```

## Filesystem
Add `wcript.sh` as a custom script to [debos-fs](https://github.com/comet-cc/debos-fs) to create the file system.
