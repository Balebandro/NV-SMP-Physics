#pragma once
// zlib (RFC 1950) / deflate (RFC 1951) decompression, for the compressed files in BSA archives. A small
// table-free decoder in the manner of zlib's puff.c: canonical Huffman codes decoded a bit at a time.
#include <cstdint>
#include <vector>

namespace inflate
{
	// Decompresses a zlib stream (2-byte header, deflate data, Adler-32) into out, whose size is the
	// expected decompressed size. False on corrupt data.
	bool Zlib(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out);
}
