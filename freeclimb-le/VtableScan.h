// VtableScan.h - encontra a "vtable" (tabela de funcoes virtuais) de uma classe C++
// compilada com o Visual C++, a partir do nome da classe, usando a informacao de
// RTTI que o compilador deixa no executavel. Codigo puro (sem SKSE nem Windows),
// para poder ser testado fora do jogo.
//
// Estrutura do RTTI do MSVC em x86:
//   TypeDescriptor : [ptr vftable][reservado][nome decorado, ex. ".?AVJumpHandler@@\0"]
//   CompleteObjectLocator (COL): [0][deslocamento][cdOffset][ptr TypeDescriptor][ptr ClassHierarchy]
//   vtable : o dword ANTES do primeiro slot aponta para o COL.
#pragma once
#include <vector>

namespace vt
{
	// Um bloco de memoria legivel: 'data' guarda os bytes que estao no endereco 'address'.
	struct Region
	{
		unsigned				address;
		unsigned				size;
		const unsigned char *	data;
	};

	struct Found
	{
		unsigned	colAddress;	// endereco do COL
		int			offset;		// 0 = vtable principal; != 0 = vtable de uma base (heranca multipla)
		unsigned	vtable;		// endereco do slot 0
		unsigned	slots;		// quantos slots seguidos apontam para codigo
	};

	struct Result
	{
		bool				typeFound;
		unsigned			typeDescriptor;
		std::vector<Found>	vtables;
	};

	// 'codeLo'..'codeHi' e o intervalo de enderecos executaveis (.text).
	Result Find(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, const char * mangledName);

	// ---- o caminho inverso: partir de uma vtable e descobrir o nome da classe ----
	struct Named
	{
		unsigned	vtable;
		unsigned	slots;
		int			offset;
		unsigned	colAddress;
		char		name[112];	// nome decorado, ex. ".?AVJumpHandler@@"
	};

	// A vtable que comeca em 'vtable' (le o ponteiro para o COL logo antes dela).
	bool Describe(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, unsigned vtable, Named & out);

	// Todas as vtables cujo "ponteiro para COL" fica no intervalo [lo, hi).
	std::vector<Named> DumpRange(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi,
								 unsigned lo, unsigned hi);

	// Quantos slots seguidos, a partir de 'vtable', apontam para codigo.
	unsigned CountSlotsAt(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, unsigned vtable);

	// Todas as classes cujo nome decorado contem 'part' (no maximo 'maxResults' tipos).
	std::vector<Named> FindByNameContaining(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi,
											const char * part, unsigned maxResults);
}
