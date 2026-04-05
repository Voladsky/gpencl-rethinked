# GPENCL Project - Agent Documentation

## Project Overview

**GPENCL** (GPU-Accelerated Encryption Library) is a high-performance, Vulkan-based GPU-accelerated encryption library that implements the ChaCha20-Poly1305 authenticated encryption with associated data (AEAD) cipher.

The project provides cryptographic file and buffer encryption/decryption capabilties using GPU compute shaders, enabling accelerated authentication, encryption, and key derivation operations. It supports password-based key derivation (PBKDF2-like with 10,000 iterations), batch multi-file operations, and device probing across multiple platforms (Windows with MSVC and Unix/Linux with Clang/GCC).

## Purpose & Use Cases

GPENCL is designed for scenarios requiring:
- **High-throughput encryption/decryption** of large files through GPU parallelization
- **File protection** with strong AEAD authentication (ChaCha20-Poly1305)
- **Multi-file batch operations** with individual password support
- **Cross-platform deployment** (Windows, Linux)
- **GPU device accessibility** through Vulkan's broad hardware support

## Technology Stack

| Component | Details |
|-----------|---------|
| **API** | Vulkan 1.0+ for GPU compute shader execution |
| **Language** | C++17 |
| **Build System** | CMake 3.16+ |
| **Shader Compiler** | shaderc (GLSL to SPIR-V compilation) |
| **Crypto Algorithms** | ChaCha20 (stream cipher), Poly1305 (MAC), PBKDF2-like KDF |
| **Platforms** | Windows (MSVC), Linux/Unix (GCC/Clang) |

## Architecture & Components

### Core Library (libgpencl)

**File: [libgpencl.h](libgpencl.h)**
- Public C++ API namespace: `GpEncl`
- **Key Functions**:
  - `init()` / `shutdown()`: Vulkan context lifecycle
  - `encrypt_file()` / `decrypt_file()`: Single file operations
  - `encrypt_files()` / `decrypt_files()`: Batch operations with per-file passwords
  - `encrypt_buffer()` / `decrypt_buffer()`: In-memory buffer operations
  - `probe_devices()`: Query available GPU devices

**File: [libgpencl.cpp](libgpencl.cpp)**
- **Vulkan State Management**:
  - Global state: VkInstance, VkPhysicalDevice, VkDevice, VkQueue
  - Compute queue family selection for shader execution
  - Command pool and descriptor pool management
  
- **Shader Management**:
  - Runtime GLSL to SPIR-V compilation via shaderc
  - Module caching for ChaCha20 and Poly1305 shaders
  - Pipeline creation with descriptor layouts
  
- **GPU Buffer Operations**:
  - Input/output buffer allocation and memory mapping
  - Key buffer for cryptographic material storage
  - Job structure packing for compute shader parameters
  - Word<->byte conversion utilities for data serialization

- **Data Structures**:
  - `GpuJob`: 16-byte aligned structure containing encryption job parameters (offsets, sizes, nonce, counter, key index)
  - `GpuKey256`: 32-byte aligned 256-bit key representation (8 × uint32)

### GPU Compute Shaders

Located in [shaders/](shaders/) directory:

**ChaCha20 Stream Cipher: [chacha20.comp](shaders/chacha20.comp)**
- Local workgroup size: 64×1×1 threads
- Implements ChaCha20 block function with quarter-round operations
- Processes multiple plaintext chunks in parallel
- Inputs: plaintext (input buffer), encryption key, nonce, counter
- Outputs: ciphertext (output buffer)
- Rotation primitives: `rotl32()` for bit manipulation

**Poly1305 Authentication: [poly1305.comp](shaders/poly1305.comp)**
- Local workgroup size: 1×1×1 (serial computation for MAC consistency)
- Generates 128-bit authentication tags over ciphertext
- Requires 64-bit integer support (`GL_ARB_gpu_shader_int64` extension)
- Inputs: ciphertext, ChaCha20 key (for OTP generation), additional authenticated data (AAD)
- Outputs: 4×uint32 authentication tag
- Ensures authenticity and integrity of encrypted data

**XOR Operation Utility: [xor.comp](shaders/xor.comp)**
- Local workgroup size: 256×1×1 threads for SIMD-style parallelism
- Performs per-byte XOR operations across multiple words
- Used for ChaCha20 stream cipher XOR phase
- Processes 256 words per dispatch in parallel

### Command-Line Interface

**File: [main.cpp](main.cpp)**
- CLI command dispatcher: `encrypt`, `decrypt`, `probe`
- **Password input modes**:
  - Single password: `-p <password>` (repeated for multiple files)
  - Password file: `-pf` or `--password-file <path>` (loads line-separated passwords)
  - Passwords auto-shuffled for batch security
- **Output naming**: Encrypted files get `.enc` suffix automatically
- Error handling and GPU initialization/shutdown orchestration

## Encrypted File Format

### Structure
```
┌──────────────────┬──────────┬───────────┬────────┐
│ Original Length  │  Nonce   │ Ciphertext│  Tag   │
│   (64-bit)       │ (96-bit) │ (variable)│(128-bit)
└──────────────────┴──────────┴───────────┴────────┘
Bytes: 0-7         8-19       20-N        N+1-N+16
```

**Components**:
1. **Bytes 0-7**: Original plaintext length (uint64_t, little-endian) — enables accurate reconstruction after GPU padding
2. **Bytes 8-19**: ChaCha20 nonce (12 random bytes, unique per encryption) — ensures semantic security
3. **Bytes 20+**: Ciphertext (plaintext XOR ChaCha20 keystream, padded to 256-byte boundary for GPU efficiency)
4. **Tail (16 bytes)**: Poly1305 authentication tag — ensures authenticity and detects tampering

## Build System

**File: [CMakeLists.txt](CMakeLists.txt)**

**Build Configuration**:
- Multi-config support for Visual Studio generators (Debug/Release)
- Platform-specific compiler flags:
  - **Windows (MSVC)**: `/W4 /Zi /Od /MD` (Debug), `/W4 /O2 /MD` (Release)
  - **Unix/Linux**: `-g -Wall -Wextra -O0` (Debug), `-Wall -Wextra -O3` (Release)
- Default build type: Debug

**Dependencies**:
- Vulkan SDK (required, auto-detected from `VULKAN_SDK` environment variable)
- shaderc library (from Vulkan SDK, cross-platform path resolution)
- C++17 standard requirement

**Output Directories**:
- Executables: `build/bin/` (config-specific subdirs on Windows)
- Libraries: `build/lib/`
- Archives: `build/lib/`

## Key Development Areas

### GPU Compute Optimization
- **Parallel workgroup sizing**: ChaCha20 uses 64-thread workgroups for throughput; Poly1305 uses 1 thread for correctness
- **Buffer memory layout**: Word-aligned storage (4-byte units) for efficient GPU access
- **Job batching**: Single `GpuJob` structure dispatched per encryption task

### Vulkan Infrastructure
- **Device selection**: Automatic physical device discovery with queue family filtering
- **Command buffer recording**: Proper synchronization between shader stages
- **Memory management**: Device-local and host-visible memory allocation strategies

### Cryptographic Correctness
- **Nonce uniqueness**: Cryptographically random 96-bit nonce per encryption ensures semantic security even with identical plaintexts
- **PBKDF2 key derivation**: 10,000 iterations from password strings to 256-bit keys
- **AEAD authentication**: Poly1305 MAC prevents ciphertext tampering

### Cross-Platform Support
- Windows MSVC compiler integration with modern C++ features
- Unix/Linux toolchain support (GCC, Clang)
- Conditional Vulkan/shaderc library path resolution per platform
- RT library handling (Release runtime library on Windows)

## Build & Execution

### Setup
```bash
# Ensure VULKAN_SDK environment variable is set
mkdir build && cd build
cmake -G "Visual Studio 17 2022" ..  # or cmake .. on Unix
cmake --build . --config Debug
```

### CLI Usage
```bash
# Single file encryption
gpencl encrypt -p "mypassword" input.txt

# Multiple files with different passwords
gpencl encrypt -p "pass1" file1.txt -p "pass2" file2.txt

# Batch encryption from password file
gpencl encrypt -pf passwords.txt file1.txt file2.txt file3.txt

# Decryption
gpencl decrypt -p "mypassword" input.txt.enc

# Query GPU devices
gpencl probe
```

## Data Flow

```
User Input (file/password)
          ↓
Password → PBKDF2 KDF → 256-bit symmetric key
File data → GPU memory allocation
          ↓
ChaCha20 GPU shader execution
          ↓
Nonce + Ciphertext → Poly1305 GPU shader
          ↓
Ciphertext + Auth Tag → File format assembly
          ↓
Encrypted output file
```

## Security Properties

- **Authenticity**: Poly1305 detects any tampering with ciphertext
- **Confidentiality**: ChaCha20 stream cipher (256-bit key, 96-bit nonce)
- **Semantic Security**: Unique random nonce per operation prevents patterns
- **Key Derivation**: PBKDF2-like (10,000 rounds) resists password cracking
- **Integrity**: File length stored to prevent padding oracle attacks

## Common Development Tasks

| Task | File(s) | Purpose |
|------|---------|---------|
| Add new encryption mode | `shaders/*.comp` | Implement alternative GPU cipher shader |
| Modify key derivation | `libgpencl.cpp` | Adjust PBKDF2 iterations or algorithm |
| Extend file format | `libgpencl.cpp` | Add metadata (timestamps, permissions) |
| Add streaming API | `libgpencl.h/cpp` | Support incremental file processing |
| Optimize GPU memory | `libgpencl.cpp` | Batch multiple files per dispatch |
| Debug GPU shaders | `shaders/*.comp` | Use Vulkan validation layers |
| Platform support | `CMakeLists.txt` | Add macOS or ARM64 Linux targets |

## Notes for Future Development

- **Shader compilation overhead**: Consider pre-compiling shaders to SPIR-V bytecode to reduce initialization time
- **Memory bandwidth**: GPU transfers may become bottleneck for small files; consider CPU fallback for <1MB files
- **Nonce management**: Current random generation adequate; consider deterministic modes for embedded use
- **Error handling**: Expand Vulkan error messages with device/capabilities info
- **Performance profiling**: GPU time ≠ wall-clock time; add timestamp queries to measure actual GPU work
- **Testing**: Unit tests for cryptographic correctness, benchmarks for throughput vs file size
