// RttiReport.cpp - passo 3. Le a informacao de tipos (RTTI) do TESV.exe ja em memoria.
// So LE memoria do proprio executavel; nao altera nada no jogo.

#include <stdlib.h>
#include <windows.h>
#include <shlobj.h>
#include <string.h>
#include <string>
#include <vector>

#include "common/IDebugLog.h"
#include "PackLoader.h"
#include "RttiReport.h"
#include "SceneHooks.h"
#include "VtableScan.h"

namespace
{
	struct Wanted
	{
		const char *	mangled;	// nome decorado do MSVC
		const char *	label;
		unsigned		known;		// vtable ja verificada no Freerun LE (0 = ainda desconhecida)
	};

	const Wanted kWanted[] =
	{
		// --- ja conhecidas do Freerun LE: servem de prova de que o metodo acerta ---
		{ ".?AVActivateHandler@@",		"ActivateHandler",		0x010D453C },
		{ ".?AVTogglePOVHandler@@",		"TogglePOVHandler",		0x010D4574 },
		{ ".?AVMovementHandler@@",		"MovementHandler",		0x010D4594 },
		{ ".?AVLookHandler@@",			"LookHandler",			0x010D45AC },
		{ ".?AVReadyWeaponHandler@@",	"ReadyWeaponHandler",	0x010D45C4 },
		{ ".?AVJumpHandler@@",			"JumpHandler",			0x010D460C },
		{ ".?AVSneakHandler@@",			"SneakHandler",			0x010D4644 },

		// --- que o FreeClimb tambem usa e ainda nao temos ---
		{ ".?AVSprintHandler@@",		"SprintHandler",		0 },
		{ ".?AVAttackBlockHandler@@",	"AttackBlockHandler",	0 },
		{ ".?AVShoutHandler@@",			"ShoutHandler",			0 },
		{ ".?AVAutoMoveHandler@@",		"AutoMoveHandler",		0 },
		{ ".?AVRunHandler@@",			"RunHandler",			0 },
		{ ".?AVToggleRunHandler@@",		"ToggleRunHandler",		0 },

		// --- objetos do jogo ---
		{ ".?AVPlayerCharacter@@",		"PlayerCharacter",		0 },
		{ ".?AVActor@@",				"Actor",				0 },
		{ ".?AVNiAVObject@@",			"NiAVObject",			0 },
		{ ".?AVNiNode@@",				"NiNode",				0 },
		{ ".?AVBSFadeNode@@",			"BSFadeNode",			0 },
		{ ".?AVBSFlattenedBoneTree@@",	"BSFlattenedBoneTree",	0 },
		{ ".?AVBSWin32KeyboardDevice@@","BSWin32KeyboardDevice",0 },
		{ ".?AVBSPCGamepadDeviceHandler@@","BSPCGamepadDeviceHandler",0 },

		// --- camera: o Freerun LE usa a vtable de entrada destes dois estados ---
		{ ".?AVThirdPersonState@@",		"ThirdPersonState",		0x010E29D4 },
		{ ".?AVFirstPersonState@@",		"FirstPersonState",		0x010E30B0 },
	};

	// Junta os blocos de memoria legiveis do executavel e o intervalo de codigo.
	void AddReadable(std::vector<vt::Region> & regions, unsigned start, unsigned size)
	{
		unsigned p = start;
		const unsigned end = start + size;

		while(p < end)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if(!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;

			const unsigned regionEnd = (unsigned)mbi.BaseAddress + (unsigned)mbi.RegionSize;
			const unsigned chunkEnd = regionEnd < end ? regionEnd : end;
			if(chunkEnd <= p) break;

			const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
			if(readable)
			{
				vt::Region r;
				r.address	= p;
				r.size		= chunkEnd - p;
				r.data		= (const unsigned char *)p;
				regions.push_back(r);
			}

			p = chunkEnd;
		}
	}

	bool Collect(std::vector<vt::Region> & regions, unsigned & codeLo, unsigned & codeHi)
	{
		const BYTE * base = (const BYTE *)GetModuleHandleA(NULL);
		if(!base) return false;

		const IMAGE_DOS_HEADER *	dos	= (const IMAGE_DOS_HEADER *)base;
		const IMAGE_NT_HEADERS32 *	nt	= (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
		const IMAGE_SECTION_HEADER * sec = IMAGE_FIRST_SECTION(nt);

		codeLo = 0xFFFFFFFF;
		codeHi = 0;

		for(unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			const unsigned start	= (unsigned)base + sec[i].VirtualAddress;
			const unsigned size		= sec[i].Misc.VirtualSize;

			if(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				if(start < codeLo)			codeLo = start;
				if(start + size > codeHi)	codeHi = start + size;
			}
			else if(sec[i].Characteristics & IMAGE_SCN_MEM_READ)
			{
				AddReadable(regions, start, size);
			}
		}

		return codeHi > codeLo && !regions.empty();
	}

	void LogNamed(const char * prefix, const vt::Named & n)
	{
		_MESSAGE("rtti: %s%08X  slots=%-3u offset=%-4d %s", prefix, n.vtable, n.slots, n.offset, n.name);
	}

	// Descobre os NOMES reais das classes, quando os que procuramos nao batem.
	void DiscoverNames(const std::vector<vt::Region> & regions, unsigned codeLo, unsigned codeHi)
	{
		// A) as vtables que o Freerun LE ja verificou: que classe sao, de facto?
		_MESSAGE("rtti: --- A) what the Freerun LE vtables really are ---");
		for(size_t w = 0; w < sizeof(kWanted) / sizeof(kWanted[0]); ++w)
		{
			if(!kWanted[w].known) continue;
			vt::Named n;
			if(vt::Describe(regions, codeLo, codeHi, kWanted[w].known, n))
				LogNamed("", n);
			else
				_MESSAGE("rtti: %08X  (not a vtable with RTTI)  [expected %s]", kWanted[w].known, kWanted[w].label);
		}

		// B) tudo o que existe na vizinhanca dessas vtables (onde ficam os outros tratadores)
		_MESSAGE("rtti: --- B) every vtable between 010D4300 and 010D4800 ---");
		const std::vector<vt::Named> range = vt::DumpRange(regions, codeLo, codeHi, 0x010D4300, 0x010D4800);
		for(size_t i = 0; i < range.size(); ++i) LogNamed("", range[i]);
		_MESSAGE("rtti: (%u vtables)", (unsigned)range.size());

		// C) classes que nao apareceram, procuradas por parte do nome
		const char * const parts[] = { "FadeNode", "Gamepad", "XInput", "Node@@" };
		for(size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p)
		{
			_MESSAGE("rtti: --- C) names containing \"%s\" ---", parts[p]);
			const std::vector<vt::Named> hits = vt::FindByNameContaining(regions, codeLo, codeHi, parts[p], 70);
			for(size_t i = 0; i < hits.size(); ++i) LogNamed("", hits[i]);
			if(hits.empty()) _MESSAGE("rtti: (none)");
		}
	}

	// ---- passo 4: despejar o inicio de cada funcao virtual para um ficheiro de texto ----
	void AppendHex(std::string & out, const unsigned char * p, unsigned n)
	{
		static const char * const digits = "0123456789ABCDEF";
		for(unsigned i = 0; i < n; ++i)
		{
			out += digits[p[i] >> 4];
			out += digits[p[i] & 15];
		}
	}

	struct DumpTarget
	{
		const char *	mangled;
		const char *	label;
		unsigned		bytes;		// quantos bytes de cada funcao
		unsigned		firstSlot;
		unsigned		lastSlot;
	};

	// Passo 15: funcoes auxiliares das passagens de atualizacao (por endereco: "#ENDERECO") e os
	// "shaders" do renderizador (onde o jogo monta as matrizes dos ossos para desenhar o corpo).
	const DumpTarget kDump[] =
	{
		{ "#00ABDEC0",						"FlatTree::Refresh(0ABDEC0)",		1500, 0, 0 },
		{ "#00ABDFC0",						"FlatTree::GetOrCreateNode(0ABDFC0)", 900, 0, 0 },
		{ "#00ABE290",						"FlatTree::Func(0ABE290)",			700,  0, 0 },
		{ "#00AB5370",						"NiAVObject::WorldData(0AB5370)",	900,  0, 0 },
		{ "#00AAF6A0",						"NiNode::Func(0AAF6A0)",			500,  0, 0 },
		{ "#00AAF430",						"NiNode::Func(0AAF430)",			500,  0, 0 },
		{ "#004202E0",						"Func(04202E0)",					300,  0, 0 },
		{ ".?AVBSLightingShader@@",			"BSLightingShader",					700,  0, 0xFFFF },
		{ ".?AVBSShader@@",					"BSShader",							700,  0, 0xFFFF },
		{ ".?AVBSEffectShader@@",			"BSEffectShader",					500,  0, 0xFFFF },
	};

	void DumpVtablesToFile(const std::vector<vt::Region> & regions, unsigned codeLo, unsigned codeHi)
	{
		std::string out;
		out.reserve(300 * 1024);
		out += "# FreeClimb LE - dump (passo 15: funcoes auxiliares e shaders)\n# formato: slot  endereco-da-funcao  bytes-iniciais-em-hex\n";
		out += "# EXT = o slot aponta para fora do TESV.exe (outro plugin interceptou-o)\n";

		unsigned classes = 0, functions = 0, external = 0;

		for(size_t k = 0; k < sizeof(kDump) / sizeof(kDump[0]); ++k)
		{
			if(kDump[k].mangled[0] == '#')
			{
				// uma funcao avulsa, dada pelo endereco ("#00ABDEC0")
				const unsigned fnAddr = (unsigned)strtoul(kDump[k].mangled + 1, NULL, 16);
				char fhead[200];
				_snprintf_s(fhead, sizeof(fhead), _TRUNCATE, "== %s vtable=00000000 slots=1 bytes=%u\n", kDump[k].label, kDump[k].bytes);
				out += fhead;
				++classes;
				if(fnAddr >= codeLo && fnAddr + kDump[k].bytes <= codeHi)
				{
					char fline[64];
					_snprintf_s(fline, sizeof(fline), _TRUNCATE, "0 %08X ", fnAddr);
					out += fline;
					AppendHex(out, (const unsigned char *)fnAddr, kDump[k].bytes);
					out += "\n";
					++functions;
				}
				continue;
			}

			vt::Result r;
			vt::Found byAddress;
			const vt::Found * primary = NULL;

			if(kDump[k].mangled[0] == '@')
			{
				// classe sem RTTI: a vtable e dada pelo endereco ("@0114D988")
				const unsigned addr = (unsigned)strtoul(kDump[k].mangled + 1, NULL, 16);
				byAddress.colAddress = 0; byAddress.offset = 0; byAddress.vtable = addr;
				byAddress.slots = vt::CountSlotsAt(regions, codeLo, codeHi, addr);
				if(byAddress.slots) primary = &byAddress;
			}
			else
			{
				r = vt::Find(regions, codeLo, codeHi, kDump[k].mangled);
				for(size_t v = 0; v < r.vtables.size(); ++v)
					if(r.vtables[v].offset == 0) { primary = &r.vtables[v]; break; }
			}

			char head[200];
			if(!primary)
			{
				_snprintf_s(head, sizeof(head), _TRUNCATE, "== %s NOT FOUND\n", kDump[k].label);
				out += head;
				continue;
			}

			++classes;
			_snprintf_s(head, sizeof(head), _TRUNCATE, "== %s vtable=%08X slots=%u bytes=%u\n",
						kDump[k].label, primary->vtable, primary->slots, kDump[k].bytes);
			out += head;

			const unsigned last = kDump[k].lastSlot < primary->slots ? kDump[k].lastSlot : primary->slots - 1;
			for(unsigned s = kDump[k].firstSlot; s <= last; ++s)
			{
				const unsigned fn = *(const unsigned *)(primary->vtable + 4 * s);
				char line[64];
				++functions;

				if(fn >= codeLo && fn + kDump[k].bytes <= codeHi)
				{
					_snprintf_s(line, sizeof(line), _TRUNCATE, "%u %08X ", s, fn);
					out += line;
					AppendHex(out, (const unsigned char *)fn, kDump[k].bytes);
					out += "\n";
				}
				else
				{
					++external;
					_snprintf_s(line, sizeof(line), _TRUNCATE, "%u %08X EXT\n", s, fn);
					out += line;
				}
			}
		}

		char path[MAX_PATH];
		if(FAILED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, SHGFP_TYPE_CURRENT, path)))
		{
			_MESSAGE("dump: could not find the Documents folder");
			return;
		}
		strcat_s(path, sizeof(path), "\\My Games\\Skyrim\\SKSE\\FreeClimbLE_vtables.txt");

		HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if(file == INVALID_HANDLE_VALUE)
		{
			_MESSAGE("dump: could not create %s", path);
			return;
		}

		DWORD written = 0;
		WriteFile(file, out.data(), (DWORD)out.size(), &written, NULL);
		CloseHandle(file);

		_MESSAGE("dump: %u classes, %u functions (%u outside TESV.exe), %u KB written to %s",
				 classes, functions, external, (unsigned)(written / 1024), path);
	}

	// ======================= passo 5: sonda do esqueleto do jogador =======================

	bool g_probeDone = false;

	bool Readable(unsigned address, unsigned size)
	{
		if(address < 0x10000 || address + size < address) return false;

		unsigned p = address;
		const unsigned end = address + size;
		while(p < end)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if(!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
			if(mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;

			const unsigned regionEnd = (unsigned)mbi.BaseAddress + (unsigned)mbi.RegionSize;
			if(regionEnd <= p) return false;
			p = regionEnd;
		}
		return true;
	}

	unsigned Rd32(unsigned a)	{ return Readable(a, 4) ? *(const unsigned *)a : 0; }
	unsigned Rd16(unsigned a)	{ return Readable(a, 2) ? *(const unsigned short *)a : 0; }
	float    RdF (unsigned a)	{ return Readable(a, 4) ? *(const float *)a : 0.0f; }

	// Nome da classe do objeto em 'obj' (a partir da vtable e do RTTI), ou "?".
	void ClassOf(const std::vector<vt::Region> & regions, unsigned lo, unsigned hi, unsigned obj, char * out, size_t size)
	{
		out[0] = '?'; out[1] = 0;
		const unsigned vtable = Rd32(obj);
		if(!vtable) return;

		vt::Named n;
		if(vt::Describe(regions, lo, hi, vtable, n))
		{
			strncpy_s(out, size, n.name, _TRUNCATE);
		}
	}

	// Texto ASCII imprimivel em 'address' (pontos para o resto).
	void AsciiAt(unsigned address, char * out, size_t size)
	{
		out[0] = 0;
		if(!address || !Readable(address, (unsigned)(size - 1))) return;

		size_t i = 0;
		for(; i + 1 < size; ++i)
		{
			const unsigned char ch = *(const unsigned char *)(address + i);
			if(ch == 0) break;
			out[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
		}
		out[i] = 0;
	}

	// Maior desvio de uma matriz 3x3 (9 floats em 'a') em relacao a uma matriz de rotacao.
	float RotationError(unsigned a)
	{
		float m[9];
		for(int i = 0; i < 9; ++i) m[i] = RdF(a + 4 * i);

		float worst = 0.0f;
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
			{
				float dot = 0.0f;
				for(int k = 0; k < 3; ++k) dot += m[i * 3 + k] * m[j * 3 + k];	// linha i . linha j
				float err = dot - (i == j ? 1.0f : 0.0f);
				if(err < 0) err = -err;
				if(err > worst) worst = err;
			}
		return worst;
	}

	// Quantos dos primeiros 51 slots a vtable 'vt' partilha com a do NiNode: um no derivado
	// (como o BSFadeNode, que nao tem RTTI no LE) partilha quase todos.
	unsigned SharedWithNiNode(unsigned vtable, unsigned niNodeVtable)
	{
		if(!vtable || !niNodeVtable || !Readable(vtable, 51 * 4)) return 0;
		unsigned same = 0;
		for(unsigned s = 0; s < 51; ++s)
			if(Rd32(vtable + 4 * s) == Rd32(niNodeVtable + 4 * s)) ++same;
		return same;
	}

	struct SceneCtx
	{
		const std::vector<vt::Region> *	regions;
		unsigned						codeLo, codeHi;
		unsigned						niNodeVtable;
		unsigned						budget;
		unsigned						flattened;	// endereco do BSFlattenedBoneTree, se achado
	};

	void PrintNode(SceneCtx & ctx, unsigned node, int depth)
	{
		if(ctx.budget == 0 || depth > 12 || !Readable(node, 0xC0)) return;
		--ctx.budget;

		const unsigned vtable = Rd32(node);
		char cls[112];
		ClassOf(*ctx.regions, ctx.codeLo, ctx.codeHi, node, cls, sizeof(cls));
		const unsigned shared = SharedWithNiNode(vtable, ctx.niNodeVtable);
		const bool isNode = strstr(cls, "Node") != NULL || shared >= 35;

		if(strstr(cls, "BSFlattenedBoneTree")) ctx.flattened = node;

		// No LE o BSFixedString aponta direto para os caracteres.
		char name[64];
		AsciiAt(Rd32(node + 0x08), name, sizeof(name));

		char indent[32];
		int pad = depth * 2; if(pad > 24) pad = 24;
		memset(indent, ' ', pad); indent[pad] = 0;

		char label[160];
		if(cls[0] == '?') _snprintf_s(label, sizeof(label), _TRUNCATE, "?(vt %08X, %u/51 shared with NiNode)", vtable, shared);
		else              _snprintf_s(label, sizeof(label), _TRUNCATE, "%s", cls);

		_MESSAGE("scene: %s%08X %s '%s' p18=%08X p1C=%08X t=(%.1f %.1f %.1f) s=%.2f",
				 indent, node, label, name, Rd32(node + 0x18), Rd32(node + 0x1C),
				 RdF(node + 0x44), RdF(node + 0x48), RdF(node + 0x4C), RdF(node + 0x50));

		if(!isNode) return;

		const unsigned data  = Rd32(node + 0xAC);
		const unsigned count = Rd16(node + 0xB4);	// m_size (confirmado: 15 filhos na raiz)
		const unsigned bound = Rd16(node + 0xB2);
		if(!data || count == 0 || count > 512 || bound > 512 || !Readable(data, bound * 4)) return;

		for(unsigned i = 0; i < bound; ++i)
		{
			const unsigned child = Rd32(data + 4 * i);
			if(child) PrintNode(ctx, child, depth + 1);
		}
	}

	// Linhas de bytes brutos (4 dwords por linha), para eu ler os campos de um objeto.
	void RawRows(const char * tag, unsigned address, unsigned bytes)
	{
		for(unsigned row = 0; row < bytes; row += 16)
		{
			if(!Readable(address + row, 16)) break;
			_MESSAGE("scene: %s +%03X  %08X %08X %08X %08X", tag, row,
					 Rd32(address + row), Rd32(address + row + 4), Rd32(address + row + 8), Rd32(address + row + 12));
		}
	}

	// Formato deduzido do LE: entradas de 0x78 bytes, num array apontado por flat+0xC0:
	//   +00 NiTransform local (52 bytes)   +34 NiTransform world (52 bytes)
	//   +68 int16 parentIndex   +6A int16 ?   +6C uint32 flags   +70 NiAVObject* node   +74 const char* name
	void PrintFlatEntry(const std::vector<vt::Region> & regions, unsigned lo, unsigned hi, unsigned base, unsigned index)
	{
		const unsigned e = base + index * 0x78;
		if(!Readable(e, 0x78)) { _MESSAGE("scene:   entry %u at %08X unreadable", index, e); return; }

		const unsigned node = Rd32(e + 0x70);
		char cls[112] = "-";
		if(node && Readable(node, 8)) ClassOf(regions, lo, hi, node, cls, sizeof(cls));
		char nodeName[48] = "", entryName[48] = "";
		if(node && Readable(node + 8, 4)) AsciiAt(Rd32(node + 8), nodeName, sizeof(nodeName));
		AsciiAt(Rd32(e + 0x74), entryName, sizeof(entryName));

		_MESSAGE("scene:   [%3u] parent=%d/%d flags=%08X node=%08X %s '%s' name='%s'  rotErr local=%.4f world=%.4f  local t=(%.1f %.1f %.1f)",
				 index, (int)(short)Rd16(e + 0x68), (int)(short)Rd16(e + 0x6A), Rd32(e + 0x6C), node, cls, nodeName, entryName,
				 RotationError(e), RotationError(e + 0x34), RdF(e + 0x24), RdF(e + 0x28), RdF(e + 0x2C));
	}

	void ProbeScene()
	{
		const DWORD t0 = GetTickCount();

		std::vector<vt::Region> regions;
		unsigned codeLo = 0, codeHi = 0;
		if(!Collect(regions, codeLo, codeHi)) { _MESSAGE("scene: could not read the executable's sections"); return; }

		const unsigned player = Rd32(0x01310588);	// g_thePlayer (SKSE LE), o mesmo que o Freerun LE usa
		if(!player) { _MESSAGE("scene: no player object yet"); return; }

		// TESObjectREFR::GetNiRootNode(firstPerson) = slot 0x6F (como no Freerun LE)
		const unsigned vtable = Rd32(player);
		if(!Readable(vtable, (0x6F + 1) * 4)) { _MESSAGE("scene: player vtable unreadable"); return; }

		typedef unsigned (__thiscall * GetRoot_t)(void *, unsigned);
		const GetRoot_t getRoot = (GetRoot_t)Rd32(vtable + 0x6F * 4);

		const unsigned third = getRoot((void *)player, 0);
		const unsigned first = getRoot((void *)player, 1);
		_MESSAGE("scene: player=%08X  third-person root=%08X  first-person root=%08X", player, third, first);
		if(!third) { _MESSAGE("scene: the player has no 3D yet (still loading?) - will try again"); return; }
		g_probeDone = true;

		const vt::Result niNode = vt::Find(regions, codeLo, codeHi, ".?AVNiNode@@");
		unsigned niNodeVtable = 0;
		for(size_t v = 0; v < niNode.vtables.size(); ++v)
			if(niNode.vtables[v].offset == 0) niNodeVtable = niNode.vtables[v].vtable;

		// A) as vtables das duas raizes (sem RTTI): quantos slots e quantos partilham com o NiNode
		const unsigned rootVt3 = Rd32(third), rootVt1 = first ? Rd32(first) : 0;
		_MESSAGE("scene: third root vtable %08X: %u slots, %u/51 shared with NiNode",
				 rootVt3, vt::CountSlotsAt(regions, codeLo, codeHi, rootVt3), SharedWithNiNode(rootVt3, niNodeVtable));
		if(rootVt1)
			_MESSAGE("scene: first root vtable %08X: %u slots, %u/51 shared with NiNode",
					 rootVt1, vt::CountSlotsAt(regions, codeLo, codeHi, rootVt1), SharedWithNiNode(rootVt1, niNodeVtable));

		// B) a arvore de nos completa
		_MESSAGE("scene: --- node tree (third person) ---");
		SceneCtx ctx;
		ctx.regions = &regions; ctx.codeLo = codeLo; ctx.codeHi = codeHi;
		ctx.niNodeVtable = niNodeVtable; ctx.budget = 400; ctx.flattened = 0;
		PrintNode(ctx, third, 0);
		_MESSAGE("scene: --- end (%u nodes printed) ---", 400 - ctx.budget);

		// C) o BSFlattenedBoneTree, se esta na arvore: bytes brutos + o que ha por tras dos ponteiros
		if(ctx.flattened)
		{
			_MESSAGE("scene: --- BSFlattenedBoneTree at %08X ---", ctx.flattened);
			RawRows("flat", ctx.flattened, 0x1A0);

			unsigned shown = 0;
			for(unsigned off = 0xB8; off < 0x1A0 && shown < 4; off += 4)
			{
				const unsigned p = Rd32(ctx.flattened + off);
				if(p < 0x10000 || !Readable(p, 0x100)) continue;
				const unsigned head = Rd32(p);
				if(head >= codeLo && head < codeHi) continue;		// um objeto com funcoes: nao e o array
				char c2[112]; ClassOf(regions, codeLo, codeHi, p, c2, sizeof(c2));
				if(c2[0] != '?') continue;							// objeto com RTTI: nao e o array
				_MESSAGE("scene: flat field +0x%03X -> %08X (first 0x100 bytes)", off, p);
				RawRows("flatdata", p, 0x100);
				++shown;
			}
		}
		else
		{
			_MESSAGE("scene: no BSFlattenedBoneTree found in the third-person tree");
		}

		if(ctx.flattened)
		{
			const unsigned countA = Rd32(ctx.flattened + 0xB8), countB = Rd32(ctx.flattened + 0xBC);
			const unsigned entries = Rd32(ctx.flattened + 0xC0);
			_MESSAGE("scene: --- flat entries: +0xB8=%u +0xBC=%u array=%08X ---", countA, countB, entries);
			if(entries && countB > 0 && countB < 4096)
			{
				for(unsigned i = 0; i < 6 && i < countB; ++i) PrintFlatEntry(regions, codeLo, codeHi, entries, i);
				_MESSAGE("scene:   ... last entries (by +0xBC):");
				for(unsigned i = countB > 4 ? countB - 4 : 0; i < countB; ++i) PrintFlatEntry(regions, codeLo, codeHi, entries, i);
				if(countA > countB && countA < 4096)
				{
					_MESSAGE("scene:   ... entries just past +0xBC (to see whether the count is right):");
					PrintFlatEntry(regions, codeLo, codeHi, entries, countB);
					PrintFlatEntry(regions, codeLo, codeHi, entries, countB + 1);
				}
			}
		}

		SceneHooksSetTarget(third, ctx.flattened);
		SceneHooksStartReport();

		if(ctx.flattened)
		{
			// Todas as entradas, uma linha cada: indice, pai, flags, no, nome. Para parar no fim real
			// do array: uma entrada sem rotacao valida e sem nome legivel.
			const unsigned entries = Rd32(ctx.flattened + 0xC0);
			unsigned shown = 0, lastGood = 0;
			std::vector<std::string> entryNames;
			_MESSAGE("scene: --- ALL flat entries (index parent flags node name) ---");
			for(unsigned i = 0; i < 1024 && entries; ++i)
			{
				const unsigned e = entries + i * 0x78;
				if(!Readable(e, 0x78)) { _MESSAGE("scene: E[%u] unreadable - stop", i); break; }

				char entryName[64];
				AsciiAt(Rd32(e + 0x74), entryName, sizeof(entryName));
				const float rotErr = RotationError(e) + RotationError(e + 0x34);
				if(entryName[0] == 0 && rotErr > 0.01f) { _MESSAGE("scene: E[%u] looks past the end (no name, rotErr=%.3f) - stop", i, rotErr); break; }

				entryNames.push_back(entryName);
				_MESSAGE("scene: E %u %d %08X %08X '%s'", i, (int)(short)Rd16(e + 0x68), Rd32(e + 0x6C), Rd32(e + 0x70), entryName);
				lastGood = i; ++shown;
			}
			_MESSAGE("scene: --- %u entries printed, last index %u ---", shown, lastGood);

			// liga cada osso canonico do FreeClimb a uma entrada da tabela, pelo nome (sem distinguir maiusculas).
			// Ficam de fora os ossos que o motor controla (raiz, folhas auxiliares, equipamento, camaras).
			int map[99];
			const int engineOwned[] = { 0, 1, 2, 3, 42, 43, 60, 61, 62, 63, 64, 65, 66, 97, 98 };
			for(int bone = 0; bone < 99; ++bone)
			{
				map[bone] = -1;
				bool skip = false;
				for(size_t k = 0; k < sizeof(engineOwned) / sizeof(engineOwned[0]); ++k) if(engineOwned[k] == bone) skip = true;
				if(skip) continue;

				const char * boneName = FC_BoneName(bone);
				for(size_t i = 0; i < entryNames.size(); ++i)
					if(_stricmp(boneName, entryNames[i].c_str()) == 0) { map[bone] = (int)i; break; }
			}
			SceneHooksBindBones(map);
		}

		_MESSAGE("scene: probe took %u ms", (unsigned)(GetTickCount() - t0));
	}

	void ScanAndLog()
	{
		const DWORD t0 = GetTickCount();

		std::vector<vt::Region> regions;
		unsigned codeLo = 0, codeHi = 0;
		if(!Collect(regions, codeLo, codeHi))
		{
			_MESSAGE("rtti: could not read the executable's sections");
			return;
		}

		// Os enderecos ja foram todos descobertos (passos 3 a 6): so falta o despejo das funcoes.
		const bool kVerboseRtti = false;
		if(!kVerboseRtti)
		{
			DumpVtablesToFile(regions, codeLo, codeHi);
			_MESSAGE("rtti: quick mode, done in %u ms", (unsigned)(GetTickCount() - t0));
			return;
		}

		unsigned totalKB = 0;
		for(size_t i = 0; i < regions.size(); ++i) totalKB += regions[i].size / 1024;
		_MESSAGE("rtti: scanning %u KB of data; code range %08X-%08X", totalKB, codeLo, codeHi);

		unsigned checked = 0, same = 0, notFound = 0;

		for(size_t w = 0; w < sizeof(kWanted) / sizeof(kWanted[0]); ++w)
		{
			const Wanted & want = kWanted[w];
			vt::Result r = vt::Find(regions, codeLo, codeHi, want.mangled);

			if(!r.typeFound)
			{
				// Tipos declarados como "struct" levam .?AU em vez de .?AV.
				char alt[160];
				_snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s", want.mangled);
				if(alt[0] == '.' && alt[1] == '?' && alt[2] == 'A' && alt[3] == 'V')
				{
					alt[3] = 'U';
					r = vt::Find(regions, codeLo, codeHi, alt);
					if(r.typeFound) _MESSAGE("rtti: %s is declared as a struct (.?AU)", want.label);
				}
			}

			if(!r.typeFound || r.vtables.empty())
			{
				++notFound;
				_MESSAGE("rtti: %-26s %s", want.label, r.typeFound ? "type found, but no vtable" : "NOT FOUND");
				continue;
			}

			bool match = false;
			for(size_t v = 0; v < r.vtables.size(); ++v)
			{
				const vt::Found & f = r.vtables[v];
				if(f.vtable == want.known) match = true;

				_MESSAGE("rtti: %-26s vtable %08X  slots=%-3u offset=%d", want.label, f.vtable, f.slots, f.offset);
			}

			if(want.known)
			{
				++checked;
				if(match) ++same;
				_MESSAGE("rtti:   cross-check with Freerun LE (%08X): %s", want.known, match ? "SAME" : "DIFFERENT");
			}
		}

		_MESSAGE("rtti: done in %u ms - cross-check %u of %u same; %u class(es) not found",
				 (unsigned)(GetTickCount() - t0), same, checked, notFound);

		DiscoverNames(regions, codeLo, codeHi);
		DumpVtablesToFile(regions, codeLo, codeHi);
	}
}

void ReportRtti()
{
	__try
	{
		ScanAndLog();
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		_MESSAGE("rtti: the scan faulted (code %08X) and was abandoned", (unsigned)GetExceptionCode());
	}
}

namespace
{
	void SafeProbe()
	{
		__try
		{
			ProbeScene();
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			_MESSAGE("scene: the probe faulted (code %08X) and was abandoned", (unsigned)GetExceptionCode());
		}
	}

	DWORD WINAPI RetryThread(LPVOID)
	{
		// O jogo manda a mensagem "save carregado" ANTES de o ecra de loading acabar (a lista de mods
		// pesada demora). Por isso espera, de 2 em 2 s (ate 4 min), que o personagem tenha corpo 3D.
		g_probeDone = false;
		Sleep(12000);
		for(int attempt = 1; attempt <= 120 && !g_probeDone; ++attempt)
		{
			_MESSAGE("scene: probing the player's skeleton (read-only), attempt %d...", attempt);
			SafeProbe();
			if(!g_probeDone) Sleep(2000);
		}
		if(!g_probeDone) _MESSAGE("scene: gave up - the player never got a 3D body");
		return 0;
	}
}

void ReportScene()
{
	const bool kSceneProbeEnabled = true;
	if(!kSceneProbeEnabled) return;

	static int calls = 0;
	if(++calls > 2) return;		// so as duas primeiras cargas de save

	// A leitura corre numa thread propria, 12 s depois: a thread principal nao pode parar
	// no momento em que o jogo carrega o save (e quando os scripts de Papyrus arrancam).
	_MESSAGE("scene: the skeleton probe is scheduled for 12 s from now (off the main thread)");
	HANDLE t = CreateThread(NULL, 0, RetryThread, NULL, 0, NULL);
	if(t) CloseHandle(t);
}
