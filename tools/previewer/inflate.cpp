#include "inflate.h"

#include <cstring>

namespace inflate
{
	namespace
	{
		struct State
		{
			const uint8_t* in;
			size_t inSize, inPos = 0;
			uint32_t bitBuf = 0;
			int bitCount = 0;
			std::vector<uint8_t>* out;
			size_t outPos = 0;
			bool error = false;

			int Bits(int need)
			{
				uint32_t v = bitBuf;
				while (bitCount < need) {
					if (inPos >= inSize) { error = true; return 0; }
					v |= (uint32_t)in[inPos++] << bitCount;
					bitCount += 8;
				}
				bitBuf = v >> need;
				bitCount -= need;
				return (int)(v & ((1u << need) - 1));
			}

			bool Put(uint8_t b)
			{
				if (outPos >= out->size()) { error = true; return false; }
				(*out)[outPos++] = b;
				return true;
			}
		};

		struct Huffman
		{
			short count[16];    // codes of each length
			short symbol[320];  // symbols ordered by code
		};

		// Canonical code from code lengths. Returns < 0 on over-subscribed lengths.
		int Build(Huffman& h, const short* length, int n)
		{
			memset(h.count, 0, sizeof h.count);
			for (int s = 0; s < n; s++) h.count[length[s]]++;
			if (h.count[0] == n) return 0;
			int left = 1;
			for (int len = 1; len < 16; len++) {
				left <<= 1;
				left -= h.count[len];
				if (left < 0) return left;
			}
			short offs[16];
			offs[1] = 0;
			for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
			for (int s = 0; s < n; s++)
				if (length[s]) h.symbol[offs[length[s]]++] = (short)s;
			return left;
		}

		int Decode(State& s, const Huffman& h)
		{
			int code = 0, first = 0, index = 0;
			for (int len = 1; len < 16; len++) {
				code |= s.Bits(1);
				if (s.error) return -1;
				int count = h.count[len];
				if (code - count < first) return h.symbol[index + (code - first)];
				index += count;
				first += count;
				first <<= 1;
				code <<= 1;
			}
			s.error = true;
			return -1;
		}

		const short kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		const short kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		const short kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		const short kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

		bool Codes(State& s, const Huffman& lencode, const Huffman& distcode)
		{
			for (;;) {
				int sym = Decode(s, lencode);
				if (sym < 0 || s.error) return false;
				if (sym < 256) {
					if (!s.Put((uint8_t)sym)) return false;
				} else if (sym == 256) {
					return true;
				} else {
					sym -= 257;
					if (sym >= 29) return false;
					int len = kLenBase[sym] + s.Bits(kLenExtra[sym]);
					int ds = Decode(s, distcode);
					if (ds < 0 || ds >= 30) return false;
					size_t dist = kDistBase[ds] + s.Bits(kDistExtra[ds]);
					if (s.error || dist > s.outPos) return false;
					for (int i = 0; i < len; i++)
						if (!s.Put((*s.out)[s.outPos - dist])) return false;
				}
			}
		}

		bool Stored(State& s)
		{
			s.bitBuf = 0;
			s.bitCount = 0;
			if (s.inPos + 4 > s.inSize) return false;
			unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
			unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
			s.inPos += 4;
			if (len != (~nlen & 0xffff) || s.inPos + len > s.inSize) return false;
			for (unsigned i = 0; i < len; i++)
				if (!s.Put(s.in[s.inPos++])) return false;
			return true;
		}

		bool Fixed(State& s)
		{
			static Huffman lencode, distcode;
			static bool built = false;
			if (!built) {
				short lengths[288];
				int sym = 0;
				for (; sym < 144; sym++) lengths[sym] = 8;
				for (; sym < 256; sym++) lengths[sym] = 9;
				for (; sym < 280; sym++) lengths[sym] = 7;
				for (; sym < 288; sym++) lengths[sym] = 8;
				Build(lencode, lengths, 288);
				for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
				Build(distcode, lengths, 30);
				built = true;
			}
			return Codes(s, lencode, distcode);
		}

		bool Dynamic(State& s)
		{
			static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			short lengths[320];
			int nlen = s.Bits(5) + 257, ndist = s.Bits(5) + 1, ncode = s.Bits(4) + 4;
			if (s.error || nlen > 286 || ndist > 30) return false;
			int index = 0;
			for (; index < ncode; index++) lengths[order[index]] = (short)s.Bits(3);
			for (; index < 19; index++) lengths[order[index]] = 0;
			Huffman lencode, distcode;
			if (Build(lencode, lengths, 19) != 0) return false;
			index = 0;
			while (index < nlen + ndist) {
				int sym = Decode(s, lencode);
				if (sym < 0 || s.error) return false;
				if (sym < 16) {
					lengths[index++] = (short)sym;
				} else {
					short len = 0;
					int rep;
					if (sym == 16) {
						if (index == 0) return false;
						len = lengths[index - 1];
						rep = 3 + s.Bits(2);
					} else if (sym == 17) {
						rep = 3 + s.Bits(3);
					} else {
						rep = 11 + s.Bits(7);
					}
					if (index + rep > nlen + ndist) return false;
					while (rep--) lengths[index++] = len;
				}
			}
			if (lengths[256] == 0) return false;
			int err = Build(lencode, lengths, nlen);
			if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
			err = Build(distcode, lengths + nlen, ndist);
			if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
			return Codes(s, lencode, distcode);
		}
	}

	bool Zlib(const uint8_t* in, size_t inSize, std::vector<uint8_t>& out)
	{
		if (inSize < 2 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) return false;
		State s;
		s.in = in + 2;
		s.inSize = inSize - 2;
		s.out = &out;
		int last;
		do {
			last = s.Bits(1);
			int type = s.Bits(2);
			if (s.error) return false;
			bool ok = type == 0 ? Stored(s) : type == 1 ? Fixed(s) : type == 2 ? Dynamic(s) : false;
			if (!ok) return false;
		} while (!last);
		return s.outPos == out.size();
	}
}
