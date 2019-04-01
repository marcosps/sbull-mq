sbull
=====

This project aims to convert the sbull block driver Single Queue (SQ) using
kernel 4.12, and later on to Multi Queue (MQ) using current master.

TODO sbull SQ:
--------------

- [x] Make sbull compile in kernel 4.12
- [x] Make it work using a simple request (blk_init_queue)
- [ ] Make it work using blk_queue_make_request
- [x] Run vfat
- [ ] Run other file systems  on sbull (ext4, xfs, ...)

TODO sbull MQ:
--------------
- [ ] Make sbull compile in current master
- [ ] Run vfat
- [ ] Run other file systems  on sbull (ext4, xfs, ...)
