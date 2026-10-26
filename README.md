# Xilinx XDMA Linux Driver

Linux kernel driver and tools for the Xilinx/AMD DMA/Bridge Subsystem for
PCI Express (XDMA) IP, forked from
[Xilinx/dma_ip_drivers](https://github.com/Xilinx/dma_ip_drivers). The other
drivers from that repository (QDMA, XVSEC, MDB5) are not included.

* [`XDMA/linux-kernel`](XDMA/linux-kernel) — kernel driver (`xdma/`),
  userspace tools (`tools/`) and test scripts (`tests/`). See
  [`XDMA/linux-kernel/readme.txt`](XDMA/linux-kernel/readme.txt).

## Changes from upstream

* Big-endian host support. The FPGA side is unchanged: descriptors, C2H
  write-back results and poll-mode write-back stay little-endian in host
  memory, and the driver converts at every access. `/dev/xdma*_user`
  read/write carry CPU-native 32-bit register values; H2C/C2H data is
  copied byte for byte.
* Builds on current kernels (tested on Linux 7.0).
* Fixes for BAR bounds checks, locking, IRQ teardown, aperture transfers,
  aio, and error-path leaks in the driver and tools.
