# ScalaAFA

*Constructing User-Space All-Flash Array Engine with Holistic Designs!*

## Introduction

*ScalaAFA* is a high-performance AFA engine built from holistic designs, which achieves high performance by embracing user-space storage stack and exploiting SSD-internal hardware resources. 

We implement the software part of ScalaAFA in SPDK, especially at `spdk/module/bdev/afa`. The hardware modification is simulated with FEMU, a popular QEMU-based simulator.

Please consider citing our paper at USENIX ATC 2024 if you use ScalaAFA. The BibTex is shown below:

```latex
@inproceedings {298505,
author = {Shushu Yi and Xiurui Pan and Qiao Li and Qiang Li and Chenxi Wang and Bo Mao and Myoungsoo Jung and Jie Zhang},
title = {{ScalaAFA}: Constructing {User-Space} {All-Flash} Array Engine with Holistic Designs},
booktitle = {2024 USENIX Annual Technical Conference (USENIX ATC 24)},
year = {2024},
isbn = {978-1-939133-41-0},
address = {Santa Clara, CA},
pages = {141--156},
url = {https://www.usenix.org/conference/atc24/presentation/yi-shushu},
publisher = {USENIX Association},
month = jul
}
```



## Build & Run

1. Clone ScalaAFA from Github: `git clone git@github.com:ChaseLab-PKU/ScalaAFA.git`

2. Install FEMU: 

   ```shell
   # Install FEMU according to https://github.com/MoatLab/FEMU/
   cd ScalaAFA/femu
   mkdir build-femu
   # Switch to the FEMU building directory
   cd build-femu
   # Copy FEMU script
   cp ../femu-scripts/femu-copy-scripts.sh .
   ./femu-copy-scripts.sh .
   # Only Debian/Ubuntu based distributions supported
   sudo ./pkgdep.sh
   # Compile & Install
   ./femu-compile.sh
   ```

3. FEMU is a QEMU-based simulator, so you need to prepare a VM image for running. You can build your own VM image, download the image provided by [FEMU](https://docs.google.com/forms/d/e/1FAIpQLSdCyNTU7n-hwW1ODJ3i_q1vmS6eTT-V3c4vCL8ouYocNLhxvA/viewform), or use the VM image provided by us.

   ```shell
   # download the image from the following address:
   https://disk.pku.edu.cn/link/AA2D750777347E4C26BBC2C1295B5AB42C
   # We will try to simplify this process with wget command ASAP.
   # Unzip the image
   sudo apt install pigz
   unpigz -d -k scalaafa-ae.qcow2.gz
   ```

4. Modify the path of your image:

   ```shell
   # Assume you are in ScalaAFA/femu/build-femu
   vi run-scalaafa.sh
   # Please modify line 6 to you image, e.g., 
   OSIMGF=/home/xxx/images/scalaafa-ae.qcow2
   ```

5. Run FEMU:

     ```shell
     # Assume you are in ScalaAFA/femu/build-femu
     ./run-scalaafa.sh
     # at least 264 GB DRAM is required 
     # if insufficient, modify the .sh file to create smaller virtual SSD.  
     ```

6. Copy software parts of ScalaAFA to VM:

   ```shell
   # We have download ScalaAFA in the VM image provided by us. Skip this step if you use that image.
   # Shell of host: assume you are in ScalaAFA/
   scp -r -P 8080 ./fio femu@localhost:/home/femu/fio  # password is femu
   scp -r -P 8080 ./spdk femu@localhost:/home/femu/spdk # password is femu
   # Connect to VM. For example, with our image:
   ssh -p8080 femu@localhost # password is femu
   # Shell of VM: assume you are in /home/femu
   mkdir ScalaAFA
   mv fio ScalaAFA/
   mv spdk ScalaAFA/
   ```

7. Compile and install ScalaAFA in VM:

   ```shell
   # Install fio
   cd ScalaAFA/fio
   ./configure
   make -j $(nproc)
   sudo make install
   # Install spdk
   cd ../spdk  						#(path/ScalaAFA/spdk)
   sudo apt-get update
   sudo scripts/pkgdep.sh
   ./configure --with-fio=/home/femu/ScalaAFA/fio  --enable-debug
   make -j $(nproc)
   ```

8. Hello, ScalaAFA:

   ```shell
   # In this example, we will create a 4+1 AFA with ScalaAFA and run a simple fio write benchmark
   # Assume you are in VM::/home/femu/ScalaAFA/spdk
   sudo HUGEMEM=10240 scripts/setup.sh
   sudo LD_PRELOAD=./build/fio/spdk_bdev ../fio/fio ./test/write.fio
   # If success, you will see our welcome slogan and fio's output:
   		███████╗ ██████╗ █████╗ ██╗      █████╗  █████╗ ███████╗ █████╗
   		██╔════╝██╔════╝██╔══██╗██║     ██╔══██╗██╔══██╗██╔════╝██╔══██╗
   		███████╗██║     ███████║██║     ███████║███████║█████╗  ███████║
   		╚════██║██║     ██╔══██║██║     ██╔══██║██╔══██║██╔══╝  ██╔══██║
   		███████║╚██████╗██║  ██║███████╗██║  ██║██║  ██║██║     ██║  ██║
   		╚══════╝ ╚═════╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝     ╚═╝  ╚═╝
   ```
