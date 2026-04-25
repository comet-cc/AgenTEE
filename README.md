# AgenTEE
Building components and reproducing the evaluation results of AgenTEE.

**Requirements**:
1) An x86 system to build components
2) Radxa Rock 5B (CAEC evaluation board)
3) Micro SD card (>= 16 GB) 
4) USB to TTL Serial Cable 

## Initializing repositories
```
repo init -u https://github.com/comet-cc/AgenTEE.git -m manifest.xml
repo sync
```
The above commands initialize repositories required to build and reproduce AgenTEE evaluation results. AgenTEE is built on top of existing code introuduced in [CAEC](https://github.com/comet-cc/CAEC). AgenTEE also uses [OpenCCA](https://github.com/opencca) as the evaluation platform. 

## Building components
1) Run the prebuild script (update submodules, apply patches, etc):
```
./manifest/prebuild.sh
```

2) Build and start the Docker container (Make sure you have the requirements installed as described [here](https://github.com/SinaAb7/opencca-build)):
```
./manifest/container.sh
```

3) Build components (edk2, CCA firmware, host and guest Linux, kvmtool, etc.) inside the container:
```
./opencca-build/scripts/build_all.sh
```
4) Download LLM (Optional): Run the following script to download GPT2 (GGUF) from huggingface. You need to provide your huggingface token for authorization.
```
# Run outside of the container, you ma need to install some python packages
./manifest/download_model.sh -m openai-community/gpt2 -t [HF_Token]
```
You can skip this step if you do not want to run data sharing benchmark.

5) Build the guest and host file systems with:
```
./manifest/build_guest_fs.sh
./manifest/build_host_fs.sh
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


**AgenTEE: Confidential LLM Agent Execution on Edge Devices**,
Sina Abdollahi, Mohammad M Maheri, Javad Forough, Amir Al Sadi, Josh Millar, David Kotz, Marios Kogias, Hamed Haddadi
**Conference**

The paper can be found [here](https://arxiv.org/pdf/2604.18231).

## Citation
If you use the code/data in your research, please cite our work as follows:
```
@article{abdollahi2026agentee,
  title={{AgenTEE: Confidential LLM Agent Execution on Edge Devices}},
  author={Abdollahi, Sina and Maheri, Mohammad M and Forough, Javad and Sadi, Amir Al and Millar, Josh and Kotz, David and Kogias, Marios and Haddadi, Hamed},
  journal={arXiv preprint arXiv:2604.18231},
  year={2026}
}
```

## Contact
In case of questions, please get in touch with [Sina Abdollahi](https://www.imperial.ac.uk/people/s.abdollahi22).
