# GPENCL
(rethinked)

## Usage example
### there are encryption and decryption modes

```./vk_chacha20poly1305 enc <plaintext_hex> [aad_hex]```

```./vk_chacha20poly1305 enc 4c616469657320616e642047656e 50515253c0c1```

as a result of encryption we'll get:

```Ciphertext: a592ca47a90a136b8194670ab9c1```

```Tag:        c8c1b7d6e71678da84ca155b37aa01d6```

to successfully decrypt ciphertext you have to use the same add_hex that was used for its encryption:

```./vk_chacha20poly1305 dec <ciphertext_hex> <tag_hex> [aad_hex]```

```./vk_chacha20poly1305 dec a592ca47a90a136b8194670ab9c1 c8c1b7d6e71678da84ca155b37aa01d6 50515253c0c1```

## Installation
compile using cmake. You need Lunar SDK for this.
