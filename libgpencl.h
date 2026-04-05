#ifndef LIBGPENCL_H
#define LIBGPENCL_H

#include <vector>
#include <string>
#include <cstdint>

// Library for GPU-accelerated encryption using ChaCha20-Poly1305

namespace GpEncl {

// Initialize the library (Vulkan context)
bool init();

// Cleanup resources
void shutdown();

// Encrypt a single file with password
bool encrypt_file(const std::string& password, const std::string& input_file, const std::string& output_file);

// Decrypt a single file with password
bool decrypt_file(const std::string& password, const std::string& input_file, const std::string& output_file);

// Encrypt multiple files with corresponding passwords
bool encrypt_files(const std::vector<std::string>& passwords, const std::vector<std::string>& input_files, const std::vector<std::string>& output_files);

// Decrypt multiple files with corresponding passwords
bool decrypt_files(const std::vector<std::string>& passwords, const std::vector<std::string>& input_files, const std::vector<std::string>& output_files);

// Encrypt a buffer with password, returns nonce + ciphertext + tag
std::vector<uint8_t> encrypt_buffer(const std::string& password, const std::vector<uint8_t>& buffer);

// Decrypt a buffer with password, returns plaintext or empty on failure
std::vector<uint8_t> decrypt_buffer(const std::string& password, const std::vector<uint8_t>& encrypted_buffer);

// Probe available GPUs
std::vector<std::string> probe_devices();

} // namespace GpEncl

#endif // LIBGPENCL_H