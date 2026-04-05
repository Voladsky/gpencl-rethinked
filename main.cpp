#include "libgpencl.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static std::vector<std::string> loadPasswordsFromFile(const std::string& path) {
    std::vector<std::string> passwords;
    std::ifstream in(path);
    if (!in) return passwords;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty())
            passwords.push_back(line);
    }

    if (!passwords.empty()) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::shuffle(passwords.begin(), passwords.end(), rng);
    }

    return passwords;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: gpencl <command> [options] <inputs>" << std::endl;
        std::cout << "Commands: encrypt, decrypt, probe" << std::endl;
        std::cout << "Options: -p <password> (can be repeated for multiple files)" << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "encrypt") {
        std::vector<std::string> passwords;
        std::vector<std::string> files;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "-p") {
                if (i + 1 < argc) {
                    passwords.push_back(argv[i + 1]);
                    i++;
                } else {
                    std::cout << "Missing password after -p" << std::endl;
                    return 1;
                }
            } else if (arg == "-pf" || arg == "--password-file") {
                if (i + 1 < argc) {
                    auto filePasswords = loadPasswordsFromFile(argv[i + 1]);
                    if (filePasswords.empty()) {
                        std::cout << "Failed to load passwords from file: " << argv[i + 1] << std::endl;
                        return 1;
                    }
                    passwords.insert(passwords.end(), filePasswords.begin(), filePasswords.end());
                    i++;
                } else {
                    std::cout << "Missing password file after " << arg << std::endl;
                    return 1;
                }
            } else {
                files.push_back(argv[i]);
            }
        }
        if (!GpEncl::init()) {
            std::cout << "Failed to initialize GPU" << std::endl;
            return 1;
        }
        std::vector<std::string> output_files;
        for (auto& f : files) {
            output_files.push_back(f + ".enc");
        }
        bool success = GpEncl::encrypt_files(passwords, files, output_files);
        GpEncl::shutdown();
        if (!success) {
            std::cout << "Encryption failed" << std::endl;
            return 1;
        }
        std::cout << "Encryption successful" << std::endl;
        return 0;
    } else if (command == "decrypt") {
        std::vector<std::string> passwords;
        std::vector<std::string> files;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "-p") {
                if (i + 1 < argc) {
                    passwords.push_back(argv[i + 1]);
                    i++;
                } else {
                    std::cout << "Missing password after -p" << std::endl;
                    return 1;
                }
            } else if (arg == "-pf" || arg == "--password-file") {
                if (i + 1 < argc) {
                    auto filePasswords = loadPasswordsFromFile(argv[i + 1]);
                    if (filePasswords.empty()) {
                        std::cout << "Failed to load passwords from file: " << argv[i + 1] << std::endl;
                        return 1;
                    }
                    passwords.insert(passwords.end(), filePasswords.begin(), filePasswords.end());
                    i++;
                } else {
                    std::cout << "Missing password file after " << arg << std::endl;
                    return 1;
                }
            } else {
                files.push_back(argv[i]);
            }
        }
        if (passwords.empty()) {
            std::cout << "No passwords provided" << std::endl;
            return 1;
        }
        if (!GpEncl::init()) {
            std::cout << "Failed to initialize GPU" << std::endl;
            return 1;
        }
        std::vector<std::string> output_files;
        for (auto& f : files) {
            if (f.size() > 4 && f.substr(f.size() - 4) == ".enc") {
                output_files.push_back(f.substr(0, f.size() - 4));
            } else {
                output_files.push_back(f + ".dec");
            }
        }
        bool success = GpEncl::decrypt_files(passwords, files, output_files);
        GpEncl::shutdown();
        if (!success) {
            std::cout << "Decryption failed" << std::endl;
            return 1;
        }
        std::cout << "Decryption successful" << std::endl;
        return 0;
    } else if (command == "probe") {
        auto devices = GpEncl::probe_devices();
        std::cout << "Available Vulkan devices:" << std::endl;
        for (auto& d : devices) {
            std::cout << "- " << d << std::endl;
        }
        return 0;
    } else {
        std::cout << "Unknown command: " << command << std::endl;
        return 1;
    }
}