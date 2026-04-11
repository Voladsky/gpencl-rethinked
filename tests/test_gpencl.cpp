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

    std::cerr << "Running corrupted buffer test...\n";
    auto corrupted_encrypted = encrypted;
    if (!corrupted_encrypted.empty()) {
        corrupted_encrypted[0] ^= 1;
    }
    auto corrupted_decrypted = GpEncl::decrypt_buffer(password, corrupted_encrypted);
    assert(corrupted_decrypted != plaintext);
    std::cerr << "Corrupted buffer test passed.\n";

    std::cerr << "Running corrupted file test...\n";
    const std::string corrupted_encrypted_path = "test_plaintext_corrupted.enc";
    const std::string corrupted_decrypted_path = "test_plaintext_corrupted.dec";
    auto corrupted_file_data = load_file(encrypted_path);
    if (!corrupted_file_data.empty()) {
        corrupted_file_data[0] ^= 1;
    }
    assert(save_file(corrupted_encrypted_path, corrupted_file_data));
    GpEncl::decrypt_file(password, corrupted_encrypted_path, corrupted_decrypted_path);
    auto corrupted_file_decrypted = load_file(corrupted_decrypted_path);
    assert(corrupted_file_decrypted != plaintext);
    std::cerr << "Corrupted file test passed.\n";

    std::cerr << "Running multi-file batch test...\n";
    const std::vector<uint8_t> batch_file1 = {'F', 'i', 'l', 'e', '1'};
    const std::vector<uint8_t> batch_file2 = {'F', 'i', 'l', 'e', '2'};
    const std::vector<uint8_t> batch_file3 = {'F', 'i', 'l', 'e', '3'};
    
    const std::string batch_input1 = "batch_test1.txt";
    const std::string batch_input2 = "batch_test2.txt";
    const std::string batch_input3 = "batch_test3.txt";
    
    const std::string batch_encrypted1 = "batch_test1.txt.enc";
    const std::string batch_encrypted2 = "batch_test2.txt.enc";
    const std::string batch_encrypted3 = "batch_test3.txt.enc";
    
    const std::string batch_decrypted1 = "batch_test1.txt.dec";
    const std::string batch_decrypted2 = "batch_test2.txt.dec";
    const std::string batch_decrypted3 = "batch_test3.txt.dec";
    
    assert(save_file(batch_input1, batch_file1));
    assert(save_file(batch_input2, batch_file2));
    assert(save_file(batch_input3, batch_file3));
    
    const std::vector<std::string> batch_passwords = {"pass1", "pass2", "pass3"};
    const std::vector<std::string> batch_inputs = {batch_input1, batch_input2, batch_input3};
    const std::vector<std::string> batch_encrypted_outputs = {batch_encrypted1, batch_encrypted2, batch_encrypted3};
    const std::vector<std::string> batch_decrypted_outputs = {batch_decrypted1, batch_decrypted2, batch_decrypted3};
    
    assert(GpEncl::encrypt_files(batch_passwords, batch_inputs, batch_encrypted_outputs));
    assert(GpEncl::decrypt_files(batch_passwords, batch_encrypted_outputs, batch_decrypted_outputs));
    
    auto batch_decrypted1_content = load_file(batch_decrypted1);
    auto batch_decrypted2_content = load_file(batch_decrypted2);
    auto batch_decrypted3_content = load_file(batch_decrypted3);
    
    assert(batch_decrypted1_content == batch_file1);
    assert(batch_decrypted2_content == batch_file2);
    assert(batch_decrypted3_content == batch_file3);
    std::cerr << "Multi-file batch test passed.\n";

    std::cerr << "Running device probing test...\n";
    auto devices = GpEncl::probe_devices();
    assert(!devices.empty());
    for (const auto& device : devices) {
        assert(!device.empty());
        std::cerr << "  Found device: " << device << "\n";
    }
    std::cerr << "Device probing test passed (" << devices.size() << " device(s) found).\n";

    std::cerr << "Running nonce uniqueness buffer test...\n";
    auto encrypted1 = GpEncl::encrypt_buffer(password, plaintext);
    auto encrypted2 = GpEncl::encrypt_buffer(password, plaintext);
    auto encrypted3 = GpEncl::encrypt_buffer(password, plaintext);
    
    assert(encrypted1 != encrypted2);
    assert(encrypted2 != encrypted3);
    assert(encrypted1 != encrypted3);
    
    assert(GpEncl::decrypt_buffer(password, encrypted1) == plaintext);
    assert(GpEncl::decrypt_buffer(password, encrypted2) == plaintext);
    assert(GpEncl::decrypt_buffer(password, encrypted3) == plaintext);
    std::cerr << "Nonce uniqueness buffer test passed.\n";

    std::cerr << "Running nonce uniqueness file test...\n";
    const std::string nonce_input = "test_nonce.txt";
    const std::string nonce_encrypted1 = "test_nonce1.enc";
    const std::string nonce_encrypted2 = "test_nonce2.enc";
    const std::string nonce_encrypted3 = "test_nonce3.enc";
    const std::string nonce_decrypted1 = "test_nonce1.dec";
    const std::string nonce_decrypted2 = "test_nonce2.dec";
    const std::string nonce_decrypted3 = "test_nonce3.dec";
    
    assert(save_file(nonce_input, plaintext));
    
    assert(GpEncl::encrypt_file(password, nonce_input, nonce_encrypted1));
    assert(GpEncl::encrypt_file(password, nonce_input, nonce_encrypted2));
    assert(GpEncl::encrypt_file(password, nonce_input, nonce_encrypted3));
    
    auto nonce_file1 = load_file(nonce_encrypted1);
    auto nonce_file2 = load_file(nonce_encrypted2);
    auto nonce_file3 = load_file(nonce_encrypted3);
    
    assert(nonce_file1 != nonce_file2);
    assert(nonce_file2 != nonce_file3);
    assert(nonce_file1 != nonce_file3);
    
    assert(GpEncl::decrypt_file(password, nonce_encrypted1, nonce_decrypted1));
    assert(GpEncl::decrypt_file(password, nonce_encrypted2, nonce_decrypted2));
    assert(GpEncl::decrypt_file(password, nonce_encrypted3, nonce_decrypted3));
    
    assert(load_file(nonce_decrypted1) == plaintext);
    assert(load_file(nonce_decrypted2) == plaintext);
    assert(load_file(nonce_decrypted3) == plaintext);
    std::cerr << "Nonce uniqueness file test passed.\n";

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
    std::remove(corrupted_encrypted_path.c_str());
    std::remove(corrupted_decrypted_path.c_str());
    std::remove(batch_input1.c_str());
    std::remove(batch_input2.c_str());
    std::remove(batch_input3.c_str());
    std::remove(batch_encrypted1.c_str());
    std::remove(batch_encrypted2.c_str());
    std::remove(batch_encrypted3.c_str());
    std::remove(batch_decrypted1.c_str());
    std::remove(batch_decrypted2.c_str());
    std::remove(batch_decrypted3.c_str());
    std::remove(nonce_input.c_str());
    std::remove(nonce_encrypted1.c_str());
    std::remove(nonce_encrypted2.c_str());
    std::remove(nonce_encrypted3.c_str());
    std::remove(nonce_decrypted1.c_str());
    std::remove(nonce_decrypted2.c_str());
    std::remove(nonce_decrypted3.c_str());

    std::cout << "All gpencl tests passed." << std::endl;
    return 0;
}
