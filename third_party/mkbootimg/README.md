# AOSP boot image tools

`mkbootimg.py`, `unpack_bootimg.py` and `gki/generate_gki_certificate.py` are copied unmodified from the Android Open Source Project
(`platform/system/tools/mkbootimg`, branch `android15-release`) and are licensed under the Apache License 2.0 (see the headers of the files).

| file | sha256 |
|---|---|
| `mkbootimg.py` | `37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c` |
| `unpack_bootimg.py` | `a9d260978a63bd06a24b6347e7dee8a28ff96639793caea15dff6aa491316308` |

The vendor_boot builder (`vendor-boot/build-vendor-boot.sh`) checks the hash of `mkbootimg.py` before it builds anything.
