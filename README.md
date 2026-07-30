<div align="center">

# Surrealra1nForge

**My personal, experimental fork of [surrealra1n](https://github.com/pwnerblu/surrealra1n)**

[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey?style=flat-square)](#requirements)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue?style=flat-square)](LICENSE)
[![Status](https://img.shields.io/badge/status-experimental-orange?style=flat-square)](#important)

[Polski](README_PL.md) · **English**

</div>

## About the project

Surrealra1nForge is my personal fork of
[surrealra1n](https://github.com/pwnerblu/surrealra1n). I use it to add and
test various features and options—including experimental ones—that may be
useful in my own workflows, research, and device testing.

Some changes may target very specific use cases and may be less thoroughly
tested than the upstream project. I currently focus mainly on A12 and A13
devices, although Forge is not limited exclusively to these platforms.

> [!IMPORTANT]
> If you only need the standard surrealra1n experience, I recommend using the
> [original project](https://github.com/pwnerblu/surrealra1n).

## Origin and thanks

Surrealra1nForge would not exist without
[**PWNBlue**](https://github.com/pwnerblu), the creator of the original
[surrealra1n](https://github.com/pwnerblu/surrealra1n) project. I would like to
thank PWNBlue and all upstream contributors for creating and maintaining the
foundation that makes this fork possible.

## Forge additions

Forge currently adds or develops the following features:

- **System Mods** — experimental modifications applied to the System Volume
  through the **System Patches** menu, including:
  - **Skip Setup** — an optional Setup Assistant modification, currently
    available only for iOS 15+ because it is designed for SSV;
  - **SSH / Dropbear** — an optional rootless Dropbear payload, currently
    available only for iOS 15+ because it is designed for SSV.
- **Custom Binpatcher** — configurable byte patches for binaries, with version
  selectors and safety checks.
- **A12/A13 experiments** — restore, patching, and device-testing changes
  developed primarily around A12 and A13 hardware.

Feature availability depends on the device, iOS version, and selected restore
configuration. Custom binary patches can be managed from **System Patches
Config → Custom Binpatches Configurator**. Their formats, selectors, safety
checks, and two-pass restore workflow are described in the
[Custom Binpatcher documentation](custom_binpatcher/README.md).

## Requirements

- macOS or Linux
- a supported device and iOS version
- the dependencies requested by the script

The inherited project supports restores for several device families. For the
upstream compatibility reference, see the original
[Supported Devices wiki](https://github.com/pwnerblu/surrealra1n/wiki/Supported-Devices).

## Installation

Clone the repository with its submodules and run the script:

```sh
git clone --branch development --recursive https://github.com/sthomsonpl/Surrealra1nForge.git
cd Surrealra1nForge
./surrealra1n.sh
```

The script retains its original `surrealra1n.sh` filename for compatibility.
Do not run it directly as root or with `sudo`.

## Important

> [!CAUTION]
> Forge contains experimental functionality. Read every warning displayed by
> the script, keep complete terminal logs, and make sure you understand the
> selected options before starting a restore.

This fork is maintained independently and is not an official upstream release.

## Credits

- [**PWNBlue**](https://github.com/pwnerblu) — creator of the original
  [surrealra1n](https://github.com/pwnerblu/surrealra1n), on which this fork is
  based.
- The libimobiledevice team, tihmstar, LukeeGD/LukeZGD, xerub, plooshi, and the
  other tool authors whose work is used by the project.
- Mineek — iPhone X restored patcher, openra1n, and seprmvr64.
- Nathan (verygenericname) — SSHRD_Script.
- bodyc1m — iPod touch 6 support and the Arch Linux/Fedora port.

## License

Surrealra1nForge is available under the Apache License 2.0. See [LICENSE](LICENSE)
and [NOTICE](NOTICE).
