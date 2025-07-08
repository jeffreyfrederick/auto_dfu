# auto_dfu

**auto_dfu** is a macOS tool that automatically triggers DFU (Device Firmware Update) mode on Apple Silicon Macs using the private AppleHPM interface. It communicates directly with the Thunderbolt controller via I2C to send low-level USB-C VDM (Vendor Defined Message) commands.

> ⚠️ **Warning:** This tool uses undocumented Apple interfaces and is intended for advanced users familiar with DFU procedures and macOS internals. Use at your own risk.

🙏 Special thanks to macvdmtool for inspiration...and most of the code. :P

---

## ✨ Features

- Detects AppleHPM devices over I2C
- Automatically enters DBMa mode
- Sends DFU VDM commands (`0x56444D73`)
- Waits for disconnection/re-enumeration
- Robust retry and stabilization logic

---

## 🔧 Requirements

- macOS 12.0+  
- Xcode command line tools  
- `AppleHPMLib` (a private Apple library — **not provided**)  
- Root privileges to access IOKit and AppleHPM  
- C++11-compatible compiler
