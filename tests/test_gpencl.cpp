#include "libgpencl.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
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

    std::cerr << "Running empty buffer test...\n";
    const std::vector<uint8_t> empty_plaintext = {};
    auto empty_encrypted = GpEncl::encrypt_buffer(password, empty_plaintext);

    auto empty_decrypted = GpEncl::decrypt_buffer(password, empty_encrypted);
    assert(empty_decrypted == empty_plaintext);
    std::cerr << "Empty buffer test passed.\n";

    std::cerr << "Running empty file test...\n";
    const std::string empty_input_path = "test_empty.txt";
    const std::string empty_encrypted_path = "test_empty.txt.enc";
    const std::string empty_decrypted_path = "test_empty.txt.dec";

    assert(save_file(empty_input_path, empty_plaintext));
    assert(GpEncl::encrypt_file(password, empty_input_path, empty_encrypted_path));
    GpEncl::decrypt_file(password, empty_encrypted_path, empty_decrypted_path);

    auto empty_file_decrypted = load_file(empty_decrypted_path);
    assert(empty_file_decrypted == empty_plaintext);
    std::cerr << "Empty file test passed.\n";

    std::cerr << "Running large file test...\n";
    const size_t large_size = 10 * 1024 * 1024;  // 10MB
    std::vector<uint8_t> large_plaintext(large_size);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (auto& byte : large_plaintext) {
        byte = dist(gen);
    }

    const std::string large_input_path = "test_large.txt";
    const std::string large_encrypted_path = "test_large.txt.enc";
    const std::string large_decrypted_path = "test_large.txt.dec";

    assert(save_file(large_input_path, large_plaintext));
    assert(GpEncl::encrypt_file(password, large_input_path, large_encrypted_path));
    assert(GpEncl::decrypt_file(password, large_encrypted_path, large_decrypted_path));

    auto large_file_decrypted = load_file(large_decrypted_path);
    assert(large_file_decrypted == large_plaintext);
    std::cerr << "Large file test passed.\n";

    std::cerr << "Running wrong password buffer test...\n";
    auto wrong_decrypted_buffer = GpEncl::decrypt_buffer("wrong-password", encrypted);
    assert(wrong_decrypted_buffer != plaintext);
    std::cerr << "Wrong password buffer test passed.\n";

    std::cerr << "Running wrong password file test...\n";
    const std::string wrong_decrypted_path = "test_plaintext_wrong.dec";
    GpEncl::decrypt_file("wrong-password", encrypted_path, wrong_decrypted_path);
    auto wrong_file_decrypted = load_file(wrong_decrypted_path);
    assert(wrong_file_decrypted != plaintext);
    std::cerr << "Wrong password file test passed.\n";

    GpEncl::shutdown();

    std::remove(input_path.c_str());
    std::remove(encrypted_path.c_str());
    std::remove(decrypted_path.c_str());
    std::remove(empty_input_path.c_str());
    std::remove(empty_encrypted_path.c_str());
    std::remove(empty_decrypted_path.c_str());
    std::remove(large_input_path.c_str());
    std::remove(large_encrypted_path.c_str());
    std::remove(large_decrypted_path.c_str());
    std::remove(wrong_decrypted_path.c_str());

    std::cout << "All gpencl tests passed." << std::endl;
    return 0;
}
