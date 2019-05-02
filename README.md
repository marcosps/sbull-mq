sbull
=====

This project aims to convert the sbull block driver to use current
implementation of Multi Queue (MQ) in more recent kernels. Some caution is used
to make the code usable for older kernels like 4.12.

This driver requires at least kernel 4.12.

TODO sbull:
--------------
- [x] Make sbull compile in current master
- [x] Run vfat
- [x] Run other file systems  on sbull (ext4, xfs, ...)
- [ ] Check capacity problem in kernel 4.12
