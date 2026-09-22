# roms/

A staging area for ROM images on the workstation. **Everything in here except
this file is gitignored**, because Acorn's ROMs are copyrighted and this
project does not redistribute them — see [“ROM images”](../README.md#rom-images)
for what you need and where to find it.

Nothing reads this directory at run time. The emulator loads ROMs from the SD
card at `/atom/roms/` (design document §11.1); this is just somewhere to keep
them, verify them and copy them from:

```sh
shasum roms/*.rom                      # check against README.md before trusting
cp roms/{akernel,abasic,afloat,dosrom}.rom /Volumes/PICOCALC/atom/roms/
```
