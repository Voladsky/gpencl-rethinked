#include "libgpencl.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::vector<uint8_t> load_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::istreambuf_iterator<char> begin(in);
    std::istreambuf_iterator<char> end;
    return std::vector<uint8_t>(begin, end);
}

static bool save_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    return out.good();
}

int main() {
    const std::string password = "test-password";
    const std::vector<uint8_t> plaintext = {
        'H', 'e', 'l', 'l', 'o', ',', ' ', 'G', 'P', 'U', ' ', 'E', 'n', 'c', 'r', 'y', 'p', 't', 'i', 'o', 'n', '\n'
    };

    if (!GpEncl::init()) {
        std::cerr << "Failed to initialize GpEncl" << std::endl;
        return 1;
    }

    std::cerr << "Running buffer round-trip test...\n";
    auto encrypted = GpEncl::encrypt_buffer(password, plaintext);
    assert(!encrypted.empty());

    auto decrypted = GpEncl::decrypt_buffer(password, encrypted);
    assert(decrypted == plaintext);

    std::cerr << "Buffer round-trip passed.\n";

    const std::string input_path = "test_plaintext.txt";
    const std::string encrypted_path = "test_plaintext.txt.enc";
    const std::string decrypted_path = "test_plaintext.txt.dec";

    assert(save_file(input_path, plaintext));

    std::cerr << "Running file encryption test...\n";
    assert(GpEncl::encrypt_file(password, input_path, encrypted_path));
    assert(GpEncl::decrypt_file(password, encrypted_path, decrypted_path));

    auto file_decrypted = load_file(decrypted_path);
    assert(file_decrypted == plaintext);

    std::cerr << "File round-trip passed.\n";

    GpEncl::shutdown();

    std::remove(input_path.c_str());
    std::remove(encrypted_path.c_str());
    std::remove(decrypted_path.c_str());

    std::cout << "All gpencl tests passed." << std::endl;
    return 0;
}
