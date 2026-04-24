# CAEC
Building components and reproducing the evaluation results of CAEC.

**Requirements**:
1) An x86 system to build components
2) Radxa Rock 5B (CAEC evaluation board)
3) Micro SD card (larger than 8 GB) 
4) USB to TTL Serial Cable 

## Initializing repositories
```
repo init -u https://github.com/comet-cc/CAEC.git -m manifest.xml
repo sync
```
The above commands initialize repositories required to build and reproduce CAEC evaluation results. We used [OpenCCA](https://github.com/opencca) as the evaluation platform. The manifest clones OpenCCA repositories to build components and flash the board (Radxa Rock 5B), along with CAEC components provided in the following repositories:
| Repository | Description |
|-------------|--------------|
| [RMM-CAEC](https://github.com/comet-cc/RMM-CAEC) | CAEC's Realm Management Monitor (RMM) |
| [Guest-linux-patches](https://github.com/comet-cc/CAEC/tree/main/patches/guest-linux) | CAEC's guest (realm) Linux patches |
| [Host-linux-patches](https://github.com/comet-cc/CAEC/tree/main/patches/host-linux) | CAEC's host (hypervisor) Linux patches |
| [External-Modules-CAEC](https://github.com/comet-cc/External-Modules-CAEC) | External kernel modules supporting CAEC |
| [kvmtool-cca-CAEC](https://github.com/comet-cc/kvmtool-cca-CAEC) | Lightweight virtual machine manager for realm VMs |
| [debos-fs](https://github.com/comet-cc/debos-fs) | File system creator for realm VMs |
| [OpenCCA-patches](https://github.com/comet-cc/CAEC/tree/main/patches) | Patches of repositories cloned from OpenCCA |

## Building components
1) Run the prebuild script (update submodules, apply patches, etc):
```
./CAEC-manifest/prebuild.sh
```

2) Build and start the Docker container (Make sure you have the requirements installed as described [here](https://github.com/SinaAb7/opencca-build)):
```
./CAEC-manifest/container.sh
```

3) Build components (edk2, CCA firmware, host and guest Linux, kvmtool, etc.) inside the container:
```
./opencca-build/scripts/build_all.sh
```
4) Download LLM (Optional): Run the following script to download GPT2 (GGUF) from huggingface. You need to provide your huggingface token for authorization.
```
# Run outside of the container, you ma need to install some python packages
./CAEC-manifest/download_model.sh -m openai-community/gpt2 -t [HF_Token]
```
You can skip this step if you do not want to run data sharing benchmark.

5) Build the guest and host file systems with:
```
./CAEC-manifest/build_guest_fs.sh
./CAEC-manifest/build_host_fs.sh
```

6) SD card preparation: Attach the SD card to the x86 build system. Find the device name under `/dev` using `lsblk` and run:
```
# Run outside of the container 
./debian-image-recipes/disk_create.sh [device_name]
```
`Hint:` If the SD card is available at `/dev/sdb`, the device_name is `sdb`. 

Insert SD card into the board.

7) Flash the board:
Hold the Maskrom key of the board, plug in the OTG port into the x86 build system, and run:
```
# Run outside of the container 
sudo ./opencca-flash/flash/flash.sh spi
```
The board is now ready. You can follow the guide [here](https://docs.radxa.com/en/rock5/rock5b/radxa-os/serial) to set up console access to the board.

## Reproducing benchmarks 

### Communication benchmark
1) Boot two realm VMs in seperate screen sessions:

create a new screen session with `screen -S master`
```
./master.sh
```
exit from the current session with `Ctrl+a d` and create a new session with `screen -S slave`
```
./slave.sh
```
2) Run the follwing scripts within the sessions:

exit from the current session with `Ctrl+a d` and log with `screen -r master`, then run:
```
./master_caec.sh
```
exit from the current session with `Ctrl+a d` and log with `screen -r slave`, then run: 
```
./slave_caec.sh
```
Now the shared memory is ready to use between two realms. To run each mode of data sharing experiment, you need to run this code on the slave side.
```
./shmem_test_[experiment] receiver [device]
```
Then, exit from the current session with `Ctrl+a d` and log with `screen -r master`, then run: 
```
./shmem_test_[experiment] sender [device]
```
[device] choices: 

`/dev/shmem0_confidential`: Confidential shared memory (CSM) between realms

`/dev/shmem0_pci`: Normal world shared memory between realms

[experiment] choices: 

`raw`: No encryption

`openssl`: Encryption of communication with OpenSSL

`mbedtls`: Encryption of communication with MbedTLS

### Data sharing benchmark
1) Boot two realm VMs in seperate screen sessions:

create a new screen session with `screen -S master`
```
./master_gpt2.sh
```
exit from the current session with `Ctrl+a d` and create a new session with `screen -S slave`
```
./slave_gpt2.sh
```
2) Run the follwing scripts within the sessions:

exit from the current session with `Ctrl+a d` and log with `screen -r master`, then run:
```
# Set the CSM from the master side
./master_caec.sh
# Write the model from the disk into the CSM and perform 6 inferences
./master_write_model_inference.sh
```

exit from the current session with `Ctrl+a d` and log with `screen -r slave`, then run: 
```
# Set the CSM from the slave side
./slave_caec.sh
# Inference test
./slave_inference.sh
```

## Paper
**CAEC: Confidential, Attestable, and Efficient Inter-CVM Communication with Arm CCA**,
Sina Abdollahi, Amir Al Sadi, Marios Kogias, David Kotz, Hamed Haddadi
**Conference**

**Abstract** -- 
The paper can be found [here](https://arxiv.org/pdf/2512.01594).

## Citation

If you use the code/data in your research, please cite our work as follows:
**google schoolar link**

## Contact
In case of questions, please get in touch with [Sina Abdollahi](https://www.imperial.ac.uk/people/s.abdollahi22).
