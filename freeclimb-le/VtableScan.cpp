#include "VtableScan.h"

#include <algorithm>
#include <string.h>

namespace vt
{
	namespace
	{
		bool Read32(const std::vector<Region> & regions, unsigned address, unsigned & out)
		{
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				if(address >= r.address && r.size >= 4 && address - r.address <= r.size - 4)
				{
					memcpy(&out, r.data + (address - r.address), 4);
					return true;
				}
			}
			return false;
		}

		bool InCode(unsigned value, unsigned lo, unsigned hi)
		{
			return value >= lo && value < hi;
		}

		// Procura o nome decorado (com o \0 final) e devolve o endereco do TypeDescriptor.
		bool FindTypeDescriptor(const std::vector<Region> & regions, const char * name, unsigned & td)
		{
			const size_t len = strlen(name) + 1;	// inclui o \0
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				if(r.size < len + 8) continue;

				for(unsigned off = 8; off + len <= r.size; ++off)
				{
					if(r.data[off] != (unsigned char)name[0]) continue;
					if(memcmp(r.data + off, name, len) != 0) continue;

					td = r.address + off - 8;
					return true;
				}
			}
			return false;
		}
	}

	Result Find(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, const char * mangledName)
	{
		Result result;
		result.typeFound = false;
		result.typeDescriptor = 0;

		if(!FindTypeDescriptor(regions, mangledName, result.typeDescriptor))
			return result;

		result.typeFound = true;

		// 1) todos os COL que apontam para este TypeDescriptor
		std::vector<unsigned> cols;
		for(size_t i = 0; i < regions.size(); ++i)
		{
			const Region & r = regions[i];
			for(unsigned off = 12; off + 4 <= r.size; off += 4)
			{
				if(((r.address + off) & 3) != 0) continue;

				unsigned value;
				memcpy(&value, r.data + off, 4);
				if(value != result.typeDescriptor) continue;

				unsigned signature = 0;
				memcpy(&signature, r.data + off - 12, 4);
				if(signature != 0) continue;	// COL de 32 bits tem assinatura 0

				cols.push_back(r.address + off - 12);
			}
		}

		// 2) para cada COL, a vtable que o referencia (o dword antes do slot 0)
		for(size_t c = 0; c < cols.size(); ++c)
		{
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				for(unsigned off = 0; off + 8 <= r.size; off += 4)
				{
					if(((r.address + off) & 3) != 0) continue;

					unsigned value;
					memcpy(&value, r.data + off, 4);
					if(value != cols[c]) continue;

					const unsigned vtable = r.address + off + 4;
					unsigned first = 0;
					if(!Read32(regions, vtable, first) || !InCode(first, codeLo, codeHi)) continue;

					Found f;
					f.colAddress = cols[c];
					f.offset = 0;
					unsigned offsetField = 0;
					if(Read32(regions, cols[c] + 4, offsetField)) f.offset = (int)offsetField;
					f.vtable = vtable;
					f.slots = 0;

					unsigned slot;
					while(f.slots < 1024 && Read32(regions, vtable + f.slots * 4, slot) && InCode(slot, codeLo, codeHi))
						++f.slots;

					result.vtables.push_back(f);
				}
			}
		}

		return result;
	}
	namespace
	{
		unsigned CountSlots(const std::vector<Region> & regions, unsigned vtable, unsigned lo, unsigned hi)
		{
			unsigned n = 0, slot;
			while(n < 1024 && Read32(regions, vtable + n * 4, slot) && InCode(slot, lo, hi)) ++n;
			return n;
		}

		// Copia uma string ASCII imprimivel terminada em \0 que esteja inteira dentro de uma regiao.
		bool ReadName(const std::vector<Region> & regions, unsigned address, char * out, size_t size)
		{
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				if(address < r.address || address - r.address >= r.size) continue;

				size_t n = 0;
				for(unsigned off = address - r.address; off < r.size && n + 1 < size; ++off, ++n)
				{
					const unsigned char ch = r.data[off];
					if(ch == 0) { out[n] = 0; return n > 4; }
					if(ch < 0x20 || ch > 0x7E) return false;
					out[n] = (char)ch;
				}
				return false;	// sem \0 a tempo
			}
			return false;
		}

		struct Candidate
		{
			unsigned	td;
			unsigned	col;
			int			offset;
			char		name[112];
		};

		bool ByTd(const Candidate & a, const Candidate & b)  { return a.td  < b.td;  }
		bool ByCol(const Candidate & a, const Candidate & b) { return a.col < b.col; }
	}

	unsigned CountSlotsAt(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, unsigned vtable)
	{
		return CountSlots(regions, vtable, codeLo, codeHi);
	}

	bool Describe(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, unsigned vtable, Named & out)
	{
		unsigned col = 0, signature = 0, td = 0, first = 0, offsetField = 0;

		if(!Read32(regions, vtable - 4, col)) return false;
		if(!Read32(regions, col, signature) || signature != 0) return false;
		if(!Read32(regions, col + 12, td)) return false;
		if(!ReadName(regions, td + 8, out.name, sizeof(out.name))) return false;
		if(out.name[0] != '.' || out.name[1] != '?' || out.name[2] != 'A') return false;
		if(!Read32(regions, vtable, first) || !InCode(first, codeLo, codeHi)) return false;

		Read32(regions, col + 4, offsetField);
		out.vtable		= vtable;
		out.colAddress	= col;
		out.offset		= (int)offsetField;
		out.slots		= CountSlots(regions, vtable, codeLo, codeHi);
		return true;
	}

	std::vector<Named> DumpRange(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi,
								 unsigned lo, unsigned hi)
	{
		std::vector<Named> found;
		for(unsigned a = lo & ~3u; a < hi; a += 4)
		{
			Named n;
			if(Describe(regions, codeLo, codeHi, a + 4, n)) found.push_back(n);
		}
		return found;
	}

	std::vector<Named> FindByNameContaining(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi,
											const char * part, unsigned maxResults)
	{
		std::vector<Candidate> types;

		// 1) todos os TypeDescriptor cujo nome contem 'part'
		for(size_t i = 0; i < regions.size() && types.size() < maxResults; ++i)
		{
			const Region & r = regions[i];
			for(unsigned off = 8; off + 8 <= r.size && types.size() < maxResults; ++off)
			{
				if(r.data[off] != '.' || r.data[off + 1] != '?' || r.data[off + 2] != 'A') continue;
				if(r.data[off + 3] != 'V' && r.data[off + 3] != 'U') continue;

				Candidate c;
				c.td = r.address + off - 8;
				c.col = 0;
				c.offset = 0;
				if(!ReadName(regions, r.address + off, c.name, sizeof(c.name))) continue;
				if(!strstr(c.name, part)) continue;
				types.push_back(c);
			}
		}
		std::sort(types.begin(), types.end(), ByTd);

		// 2) numa unica passagem: todos os COL que apontam para esses tipos
		std::vector<Candidate> cols;
		for(size_t i = 0; i < regions.size() && !types.empty(); ++i)
		{
			const Region & r = regions[i];
			for(unsigned off = 12; off + 4 <= r.size; off += 4)
			{
				if(((r.address + off) & 3) != 0) continue;
				unsigned value;
				memcpy(&value, r.data + off, 4);

				Candidate key; key.td = value;
				std::vector<Candidate>::iterator it = std::lower_bound(types.begin(), types.end(), key, ByTd);
				if(it == types.end() || it->td != value) continue;

				unsigned signature;
				memcpy(&signature, r.data + off - 12, 4);
				if(signature != 0) continue;

				Candidate c = *it;
				c.col = r.address + off - 12;
				unsigned offsetField;
				memcpy(&offsetField, r.data + off - 8, 4);
				c.offset = (int)offsetField;
				cols.push_back(c);
			}
		}
		std::sort(cols.begin(), cols.end(), ByCol);

		// 3) numa unica passagem: a vtable que referencia cada COL
		std::vector<Named> result;
		for(size_t i = 0; i < regions.size() && !cols.empty(); ++i)
		{
			const Region & r = regions[i];
			for(unsigned off = 0; off + 8 <= r.size; off += 4)
			{
				if(((r.address + off) & 3) != 0) continue;
				unsigned value;
				memcpy(&value, r.data + off, 4);

				Candidate key; key.col = value;
				std::vector<Candidate>::iterator it = std::lower_bound(cols.begin(), cols.end(), key, ByCol);
				if(it == cols.end() || it->col != value) continue;

				const unsigned vtable = r.address + off + 4;
				unsigned first = 0;
				if(!Read32(regions, vtable, first) || !InCode(first, codeLo, codeHi)) continue;

				Named n;
				n.vtable = vtable;
				n.colAddress = it->col;
				n.offset = it->offset;
				n.slots = CountSlots(regions, vtable, codeLo, codeHi);
				memcpy(n.name, it->name, sizeof(n.name));
				result.push_back(n);
			}
		}
		return result;
	}
}
