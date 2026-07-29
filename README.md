# surrealra1n 

A tethered downgrade tool for some A7/A8(X) devices, all A11 devices and A12/A13 iPhones.

Supports macOS and Linux

For surrealra1n support, join the [surrealra1n](https://discord.gg/kDXVHhTQs2) Discord Server

# Compatible devices and versions:

View the [Supported Devices](https://github.com/pwnerblu/surrealra1n/wiki/Supported-Devices) section in the wiki for more information

# Usage:

Download surrealra1n [here](https://github.com/pwnerblu/surrealra1n/releases/latest) or clone it using git:
```
git clone -b development https://github.com/pwnerblu/surrealra1n
```
Extract the zip file and open a terminal window to the folder that contains surrealra1n, then launch it using the command: ```./surrealra1n.sh```.

# Custom Binpatches

Custom binary patches can be configured from the System Patches Config menu.
The target IPSW is inspected to select either the sealed or unsealed System
volume workflow. Skip Setup and SSH require a sealed System Volume on iOS 15+;
Custom Binpatches also support older targets. See the
[Custom Binpatcher documentation](custom_binpatcher/README.md) for the patch
format, version selectors, template, safety checks, and two-pass restore
workflow.

# Thanks to:

libimobiledevice team, tihmstar, LukeeGD/LukeZGD, xerub, plooshi, etc! (for the tools it has to download)

Mineek - iPhone X restored patcher, used for ipx restores 14.3-15.6.1 (my fork of the patcher is used for seprmvr64 restores on A8+), openra1n, and seprmvr64

Nathan (verygenericname) - SSHRD_Script









