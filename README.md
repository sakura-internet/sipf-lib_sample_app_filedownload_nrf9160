# sipf-lib_sample_app_filedownload_nrf9160

## Getting start

### About

This software is `File put sample' for Sakura's MONOPLATFORM.  
Target divice are [Nordic nRF9160DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9160-DK) and [Nordic Thingy:91](https://www.nordicsemi.com/Products/Development-hardware/Nordic-Thingy-91).

### Install nRF Connect SDK

See [nRF Connect SDK Getting started](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html).  
If you want to install the development environment quickly, see [Installing the nRF Connect SDK through nRF Connect for Desktop](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/gs_assistant.html#gs-assistant).

Using nRF Connect SDK v2.3.0 .

### Clone this repository

```
git clone --recursive https://github.com/sakura-internet/sipf-lib_sample_app_filedownload_nrf9160.git
cd sipf-lib_sample_app_tx_nrf9160
```

This program is depend to [sipf-lib_nrfconnect](https://github.com/sakura-internet/sipf-lib_nrfconnect) library.  
You must specify the `--recursive` option.

### Clean

```
rm -rf build
```

### Build

Use `west build` for build.

```
west build -b [board]
```

board

- nrf9160dk_nrf9160_ns
- thingy91_nrf9160_ns
- scm-ltem1nrf_nrf9160_ns

For Nordic nRF9160DK
```
west build -b nrf9160dk_nrf9160_ns
```

### Flash

`nrfjprog` is required.

```
west flash
```

OR

Write the HEX image file 'build/zephyr/merged.hex' using nRF Connect `Programmer' application.

---
Please refer to the [さくらのモノプラットフォーム Client library for nRFConnect Wiki(Japanese)](https://github.com/sakura-internet/sipf-lib_nrfconnect/wiki) for library specifications.
