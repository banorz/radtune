# RadeonOC-Setter (ADLX)

A lightweight command-line tool to tune AMD GPU parameters using the AMD ADLX SDK. It allows for quick overclocking, undervolting, and power limit adjustments, as well as loading tuning profiles from XML files.

## Features

- **Query GPU Info**: List all detected AMD GPUs and their current tuning settings.
- **Manual Tuning**: Set Core Frequency, VRAM Frequency, and Power Limits via CLI arguments.
- **XML Profile Support**: Load and apply factory or custom tuning profiles (`.xml`) exported from AMD Software: Adrenalin Edition.
- **Modern ADLX Integration**: Uses the latest ADLX SDK for compatibility with RDNA 2 and RDNA 3 architectures.

## Prerequisites

- **Windows 10/11**
- **AMD Radeon GPU** with supported drivers.
- **AMD ADLX SDK** (included in `adlx_sdk/` directory).
- **CMake** (v3.10+)
- **Visual Studio 2022** (with C++ development workload).

## How to Build

### Using the provided batch script
Simply run:
```bash
build.bat
```

### Manual Build
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Usage

### 1. List GPUs and Current Settings
```bash
RadeonOCSetter.exe -list
```

### 2. Apply Manual Tuning
```bash
# gpu: index of the GPU (default 0)
# core: Max GPU Frequency in MHz
# vram: Max VRAM Frequency in MHz
# power: Power Limit percentage (e.g., 15 for +15%)
RadeonOCSetter.exe -set gpu=0 core=2500 vram=2100 power=15
```

### 3. Load Tuning Profile from XML
```bash
# Loads settings from an exported AMD profile
RadeonOCSetter.exe -load "C:\path\to\performance_profile.xml"
```

## Project Structure

- `src/`: Source code (`.cpp`) and headers (`.h`).
- `adlx_sdk/`: The AMD ADLX SDK files.
- `CMakeLists.txt`: Build configuration.
- `.gitignore`: Excludes build artifacts and local settings.

## License

This project is for educational/demonstration purposes. ADLX SDK is subject to AMD's license terms.
