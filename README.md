sbull
=====

This project aims to convert the sbull block driver Single Queue (SQ) using
kernel 4.12, and Multi Queue (MQ) using current master.

By default, sbull-mq is build. If you want to build sbull Single Queue, execute
like bellow:

```sh
make SQ=1
```

TODO sbull SQ:
--------------

- [x] Make sbull compile in kernel 4.12
- [x] Run vfat
- [x] Run other file systems  on sbull (ext4, xfs, ...)

TODO sbull MQ:
--------------
- [x] Make sbull compile in current master
- [x] Run vfat
- [x] Run other file systems  on sbull (ext4, xfs, ...)
