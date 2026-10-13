// SceneHooks.cpp - passo 8. Ver SceneHooks.h.
//
// Posicoes e numero de argumentos (confirmados no despejo de funcoes do passo 7):
//   0x29 passagem "para baixo"      2 args    0x2A passagem "selecionada"  2 args
//   0x2B passagem "rigida"          2 args    0x2E so transformacao        1 arg
//   0x2D dados do mundo             1 arg
// Convencao: thiscall (this em ECX), a funcao chamada limpa a pilha ("ret N").

#include <math.h>
#include <string.h>
#include <windows.h>

#include "common/IDebugLog.h"
#include "PackLoader.h"
#include "SceneHooks.h"

namespace
{
	enum Table { kNiNode, kBSFadeNode, kFlatTree, kNiAVObject, kTables };
	enum Kind  { kDownward, kSelected, kRigid, kTransform, kWorld, kKinds };

	const unsigned	kVtable[kTables]	= { 0x011171A4, 0x0114D988, 0x011186A4, 0x011172B4 };
	const char *	kTableName[kTables]	= { "NiNode", "BSFadeNode", "BSFlattenedBoneTree", "NiAVObject" };
	const unsigned	kSlot[kKinds]		= { 0x29, 0x2A, 0x2B, 0x2E, 0x2D };
	const char *	kKindName[kKinds]	= { "downward(0x29)", "selected(0x2A)", "rigid(0x2B)", "transform(0x2E)", "world(0x2D)" };

	// So conta o esqueleto do jogador (a raiz e a tabela achatada, em separado):
	// contar todas as chamadas do jogo custava ~300 mil operacoes atomicas por segundo.
	volatile LONG		g_rootCalls[kKinds];
	volatile LONG		g_flatCalls[kKinds];
	volatile unsigned	g_playerRoot = 0, g_playerFlat = 0;
	volatile DWORD		g_threads[8];
	volatile LONG		g_threadCount = 0;
	DWORD				g_loadThread = 0;

	void NoteThread()
	{
		const DWORD id = GetCurrentThreadId();
		const LONG known = g_threadCount < 8 ? g_threadCount : 8;
		for(LONG i = 0; i < known; ++i)
			if(g_threads[i] == id) return;

		const LONG slot = InterlockedIncrement(&g_threadCount) - 1;
		if(slot < 8) g_threads[slot] = id;
	}

	inline void Count(int kind, void * self)
	{
		const unsigned s = (unsigned)self;
		if(s == g_playerFlat && s)		{ InterlockedIncrement(&g_flatCalls[kind]); NoteThread(); }
		else if(s == g_playerRoot && s)	{ InterlockedIncrement(&g_rootCalls[kind]); NoteThread(); }
	}

	// ---- Passo 17: aplicar uma POSE REAL do FreeClimb ao esqueleto do jogador ----
	// O jogo, no fim da passagem (0ABDEC0), copia a transformacao mundial de cada no para a tabela achatada,
	// e o desenho le essas posicoes. Por isso: DEPOIS da passagem, recalculamos as posicoes mundiais da
	// hierarquia inteira (645 entradas, pais antes dos filhos) e escrevemo-las na entrada e no no.
	// Para os ossos canonicos a rotacao local vem da pose; o resto mantem a transformacao local atual.
	volatile DWORD		g_poseStart = 0, g_poseEnd = 0;
	volatile LONG		g_poseReady = 0, g_poseFrames = 0;
	float				g_pose[99 * 10];
	int					g_boneEntry[99];
	volatile LONG		g_fkCheckRequested = 0, g_fkCheckDone = 0;
	float				g_fkMaxT = 0, g_fkMaxRotDeg = 0;
	unsigned			g_fkEntries = 0, g_fkMismatch = 0;
	volatile LONG		g_inPose = 0;

	bool ReadableMem(unsigned address, unsigned size)
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

	bool FlatArray(unsigned flat, unsigned * entries, unsigned * count)
	{
		if(!ReadableMem(flat + 0xB8, 12)) return false;
		*count = *(const unsigned *)(flat + 0xB8);
		*entries = *(const unsigned *)(flat + 0xC0);
		return *entries && *count > 0 && *count <= 1024;
	}

	struct Tr { float r[9]; float t[3]; float s; };		// NiTransform: rotacao 3x3 (linhas), translacao, escala

	// como o jogo multiplica transformacoes: R = Rp*Rl ; T = Tp + sp*(Rp*Tl) ; S = sp*sl
	void Mul(const Tr & p, const Tr & l, Tr * out)
	{
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
				out->r[i * 3 + j] = p.r[i * 3 + 0] * l.r[0 * 3 + j] + p.r[i * 3 + 1] * l.r[1 * 3 + j] + p.r[i * 3 + 2] * l.r[2 * 3 + j];
		for(int i = 0; i < 3; ++i)
			out->t[i] = p.t[i] + p.s * (p.r[i * 3 + 0] * l.t[0] + p.r[i * 3 + 1] * l.t[1] + p.r[i * 3 + 2] * l.t[2]);
		out->s = p.s * l.s;
	}

	// quaternio (x y z w) -> matriz; as COLUNAS sao os eixos rodados (a mesma convencao do FreeClimb/NiMatrix3)
	void FromQuat(const float * q, float * r)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		r[0] = 1 - 2 * (y * y + z * z);	r[1] = 2 * (x * y - z * w);		r[2] = 2 * (x * z + y * w);
		r[3] = 2 * (x * y + z * w);		r[4] = 1 - 2 * (x * x + z * z);	r[5] = 2 * (y * z - x * w);
		r[6] = 2 * (x * z - y * w);		r[7] = 2 * (y * z + x * w);		r[8] = 1 - 2 * (x * x + y * y);
	}

	float RotAngleDeg(const float * a, const float * b)
	{
		float tr = 0;
		for(int i = 0; i < 9; ++i) tr += a[i] * b[i];
		float c = (tr - 1.0f) * 0.5f;
		if(c > 1.0f) c = 1.0f;
		if(c < -1.0f) c = -1.0f;
		return acosf(c) * 57.29578f;
	}

	// A transformacao local ATUAL de uma entrada: do no, se tem no (o no e o que o jogo anima); senao da entrada.
	void LiveLocal(unsigned e, Tr * out)
	{
		const unsigned node = *(const unsigned *)(e + 0x70);
		if(node && ReadableMem(node + 0x20, 0x34))
		{
			memcpy(out->r, (const void *)(node + 0x20), 36);
			memcpy(out->t, (const void *)(node + 0x44), 12);
			out->s = *(const float *)(node + 0x50);
		}
		else
		{
			memcpy(out->r, (const void *)(e + 0x00), 36);
			memcpy(out->t, (const void *)(e + 0x24), 12);
			out->s = *(const float *)(e + 0x30);
		}
	}

	void ReadWorld(unsigned base, unsigned rotOff, unsigned tOff, unsigned sOff, Tr * out)
	{
		memcpy(out->r, (const void *)(base + rotOff), 36);
		memcpy(out->t, (const void *)(base + tOff), 12);
		out->s = *(const float *)(base + sOff);
	}

	void WriteWorld(unsigned base, unsigned rotOff, unsigned tOff, unsigned sOff, const Tr & w)
	{
		memcpy((void *)(base + rotOff), w.r, 36);
		memcpy((void *)(base + tOff), w.t, 12);
		*(float *)(base + sOff) = w.s;
	}

	// ---- reproducao no tempo (passo 18) ----
	volatile LONG		g_playing = 0, g_playFrames = 0, g_restReady = 0;
	int					g_playMotion = 0;
	float				g_playSeconds = 1.0f;
	volatile DWORD		g_playStart = 0;
	float				g_rest[99 * 10];
	float				g_scaleY = 1.0f, g_scaleZ = 1.0f;
	SceneFinishCallback	g_finishCb = NULL;
	SceneTickCallback	g_tickCb = NULL;
	volatile LONG		g_manual = 0;
	int					g_manualMotion = 1;
	float				g_manualPhase = 0.0f;
	unsigned			g_fkBadIdx[12], g_fkBadCount = 0;
	float				g_fkRecT = 0, g_fkRecRot = 0;

	Tr g_live[1024], g_world[1024], g_world2[1024];

	// inversa de uma transformacao: R' = R^T, s' = 1/s, t' = -s' * R' * t
	void Inv(const Tr & a, Tr * out)
	{
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
				out->r[i * 3 + j] = a.r[j * 3 + i];
		out->s = 1.0f / a.s;
		for(int i = 0; i < 3; ++i)
			out->t[i] = -out->s * (out->r[i * 3 + 0] * a.t[0] + out->r[i * 3 + 1] * a.t[1] + out->r[i * 3 + 2] * a.t[2]);
	}

	// Calcula as posicoes mundiais de todas as entradas.
	// A transformacao local de cada osso e a EFETIVA: a que reproduz exatamente o mundo que o jogo acabou de calcular
	// (inversa(mundo do pai) * mundo do osso). Assim, ossos de fisica ou com sinalizadores especiais ficam como estao.
	// Se 'usePose', os ossos canonicos recebem a rotacao da pose; o osso do centro do corpo (4) recebe tambem o
	// deslocamento da pose em relacao ao repouso autoral (e ai que esta a subida de uma escalada).
	bool ComputeWorlds(unsigned flat, bool usePose, unsigned entries, unsigned count)
	{
		int boneOfEntry[1024];
		for(unsigned i = 0; i < count; ++i) boneOfEntry[i] = -1;
		if(usePose)
			for(int b = 0; b < 99; ++b)
				if(g_boneEntry[b] >= 0 && (unsigned)g_boneEntry[b] < count) boneOfEntry[g_boneEntry[b]] = b;

		Tr root;
		ReadWorld(flat, 0x54, 0x78, 0x84, &root);		// a propria tabela e o "pai" das entradas de topo

		for(unsigned i = 0; i < count; ++i)
		{
			const unsigned e = entries + i * 0x78;
			if(!ReadableMem(e, 0x78)) return false;
			ReadWorld(e, 0x34, 0x58, 0x64, &g_live[i]);
		}

		for(unsigned i = 0; i < count; ++i)
		{
			const unsigned e = entries + i * 0x78;
			const int parent = (int)(short)(*(const unsigned short *)(e + 0x68));
			if(parent >= (int)i) return false;											// ordem inesperada: nao arriscar

			Tr inv, local;
			Inv(parent >= 0 ? g_live[parent] : root, &inv);
			Mul(inv, g_live[i], &local);												// local efetiva

			const int bone = boneOfEntry[i];
			if(bone >= 0)
			{
				const float * p = g_pose + bone * 10;
				FromQuat(p + 3, local.r);
				if(bone == 4 && g_restReady)											// centro do corpo: soma o deslocamento da pose
				{
					local.t[0] += (p[0] - g_rest[bone * 10 + 0]) * ((g_scaleY == 0.0f && g_scaleZ == 0.0f) ? 0.0f : 1.0f);
					local.t[1] += (p[1] - g_rest[bone * 10 + 1]) * g_scaleY;			// para a frente, ajustado a borda
					local.t[2] += (p[2] - g_rest[bone * 10 + 2]) * g_scaleZ;			// para cima, ajustado a borda
				}
			}
			Mul(parent >= 0 ? g_world[parent] : root, local, &g_world[i]);
		}
		return true;
	}

	// Depois da atualizacao do esqueleto do jogador.
	void PostPass(unsigned flat)
	{
		if(g_tickCb) g_tickCb();									// a logica de jogo decide a pose deste frame

		const bool manual = g_manual != 0;
		const bool playing = (g_playing || manual) && g_restReady;
		if(!playing && !g_fkCheckRequested) return;

		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count)) return;
		if(InterlockedCompareExchange(&g_inPose, 1, 0) != 0) return;

		bool finished = false;
		if(playing)
		{
			int motion = g_playMotion;
			float phase;
			if(manual)
			{
				motion = g_manualMotion;
				phase = g_manualPhase;
			}
			else
			{
				phase = (float)(GetTickCount() - g_playStart) / 1000.0f / g_playSeconds;
				if(phase >= 1.0f) { phase = 1.0f; finished = true; }
			}

			if(FC_SampleMotion(motion, phase, g_pose) && ComputeWorlds(flat, true, entries, count))
			{
				for(unsigned i = 0; i < count; ++i)
				{
					const unsigned e = entries + i * 0x78;
					WriteWorld(e, 0x34, 0x58, 0x64, g_world[i]);						// entrada: mundo em +0x34
					const unsigned node = *(const unsigned *)(e + 0x70);
					if(node && ReadableMem(node + 0x54, 0x34)) WriteWorld(node, 0x54, 0x78, 0x84, g_world[i]);	// no: mundo em +0x54
				}
				InterlockedIncrement(&g_playFrames);
			}
		}
		else if(g_fkCheckRequested && ComputeWorlds(flat, false, entries, count))
		{
			// (A) a reconstrucao a partir das locais efetivas reproduz o mundo do jogo? (testa Mul e Inv)
			float recT = 0, recRot = 0;
			for(unsigned i = 0; i < count; ++i)
			{
				float dt = 0;
				for(int k = 0; k < 3; ++k) { const float d = fabsf(g_live[i].t[k] - g_world[i].t[k]); if(d > dt) dt = d; }
				const float dr = RotAngleDeg(g_live[i].r, g_world[i].r);
				if(dt > recT) recT = dt;
				if(dr > recRot) recRot = dr;
			}

			// (B) a hierarquia com as locais "diretas" (do no ou da entrada) bate com o mundo do jogo? Quais nao?
			Tr root; ReadWorld(flat, 0x54, 0x78, 0x84, &root);
			unsigned bad = 0, kept = 0; float maxT = 0, maxRot = 0;
			for(unsigned i = 0; i < count; ++i)
			{
				const unsigned e = entries + i * 0x78;
				Tr local; LiveLocal(e, &local);
				const int parent = (int)(short)(*(const unsigned short *)(e + 0x68));
				Mul(parent >= 0 ? g_world2[parent] : root, local, &g_world2[i]);

				float dt = 0;
				for(int k = 0; k < 3; ++k) { const float d = fabsf(g_live[i].t[k] - g_world2[i].t[k]); if(d > dt) dt = d; }
				const float dr = RotAngleDeg(g_live[i].r, g_world2[i].r);
				if(dt > maxT) maxT = dt;
				if(dr > maxRot) maxRot = dr;
				if(dt > 0.5f || dr > 1.0f) { if(kept < 12) g_fkBadIdx[kept++] = i; ++bad; }
			}
			g_fkRecT = recT; g_fkRecRot = recRot;
			g_fkMaxT = maxT; g_fkMaxRotDeg = maxRot; g_fkEntries = count; g_fkMismatch = bad; g_fkBadCount = kept;
			InterlockedExchange(&g_fkCheckRequested, 0);
			InterlockedExchange(&g_fkCheckDone, 1);
		}

		InterlockedExchange(&g_inPose, 0);

		if(finished)
		{
			InterlockedExchange(&g_playing, 0);
			SceneFinishCallback cb = g_finishCb;
			g_finishCb = NULL;
			if(cb) cb();
		}
	}

	typedef void (__thiscall * Fn2)(void *, void *, unsigned);	// (this, dados, flags)  ret 8
	typedef void (__thiscall * Fn1)(void *, void *);			// (this, dados)         ret 4

	template<int T, int K> struct Pass2
	{
		static Fn2 original;
		static void __fastcall call(void * self, void *, void * data, unsigned flags)
		{
			Count(K, self);
			original(self, data, flags);
		}
	};
	template<int T, int K> Fn2 Pass2<T, K>::original = NULL;

	// A passagem "para baixo" da tabela achatada leva a experiencia (so para o esqueleto do jogador).
	template<> struct Pass2<kFlatTree, kDownward>
	{
		static Fn2 original;
		static void __fastcall call(void * self, void *, void * data, unsigned flags)
		{
			Count(kDownward, self);
			const bool player = ((unsigned)self == g_playerFlat && g_playerFlat);

			original(self, data, flags);
			if(player) PostPass((unsigned)self);
		}
	};
	Fn2 Pass2<kFlatTree, kDownward>::original = NULL;

	template<int T, int K> struct Pass1
	{
		static Fn1 original;
		static void __fastcall call(void * self, void *, void * data)
		{
			Count(K, self);
			original(self, data);
		}
	};
	template<int T, int K> Fn1 Pass1<T, K>::original = NULL;

	// Escreve o ponteiro novo na vtable, de forma atomica, e devolve o antigo.
	template<class Fn> bool Patch(int table, int kind, void * hook, Fn * original)
	{
		volatile LONG * entry = (volatile LONG *)(kVtable[table] + 4 * kSlot[kind]);

		const unsigned previous = (unsigned)*entry;
		HMODULE owner = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCSTR)previous, &owner);
		const bool ours = owner == GetModuleHandleA(NULL);

		*original = (Fn)previous;

		DWORD old = 0;
		if(!VirtualProtect((LPVOID)entry, 4, PAGE_READWRITE, &old))
		{
			_MESSAGE("hooks: %s %s: VirtualProtect failed", kTableName[table], kKindName[kind]);
			return false;
		}
		InterlockedExchange(entry, (LONG)hook);
		VirtualProtect((LPVOID)entry, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (LPCVOID)entry, 4);

		if(!ours) _MESSAGE("hooks: %s %s: the slot already belonged to another module (%08X) - chained", kTableName[table], kKindName[kind], previous);
		return true;
	}

	template<int T, int K> bool Install2() { return Patch(T, K, (void *)&Pass2<T, K>::call, &Pass2<T, K>::original); }
	template<int T, int K> bool Install1() { return Patch(T, K, (void *)&Pass1<T, K>::call, &Pass1<T, K>::original); }

	DWORD WINAPI ReportThread(LPVOID)
	{
		LONG lastRoot[kKinds] = { 0 }, lastFlat[kKinds] = { 0 };

		for(int round = 1; round <= 3; ++round)
		{
			Sleep(5000);

			char line[400];
			int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "hooks: after %d s, calls per second (root / flat tree):", round * 5);
			for(int k = 0; k < kKinds && n > 0; ++k)
			{
				const LONG r = g_rootCalls[k], f = g_flatCalls[k];
				n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "  %s %ld/%ld", kKindName[k], (r - lastRoot[k]) / 5, (f - lastFlat[k]) / 5);
				lastRoot[k] = r; lastFlat[k] = f;
			}
			_MESSAGE("%s", line);

			const LONG nThreads = g_threadCount < 8 ? g_threadCount : 8;
			char threads[160] = "";
			int m = 0;
			for(LONG i = 0; i < nThreads && m >= 0; ++i)
				m += _snprintf_s(threads + m, sizeof(threads) - m, _TRUNCATE, " %lu", (unsigned long)g_threads[i]);
			_MESSAGE("hooks:   threads that updated the player's skeleton:%s   (plugin was loaded on thread %lu)",
					 threads, (unsigned long)g_loadThread);
		}
		return 0;
	}
}

void InstallSceneHooks()
{
	g_loadThread = GetCurrentThreadId();

	// Os mesmos 16 pontos que o FreeClimb usa no SE: 3 tabelas x 3 passagens, 3 de transformacao, 4 de mundo.
	unsigned installed = 0;
	installed += Install2<kNiNode,     kDownward>();	installed += Install2<kNiNode,     kSelected>();	installed += Install2<kNiNode,     kRigid>();
	installed += Install2<kBSFadeNode, kDownward>();	installed += Install2<kBSFadeNode, kSelected>();	installed += Install2<kBSFadeNode, kRigid>();
	installed += Install2<kFlatTree,   kDownward>();	installed += Install2<kFlatTree,   kSelected>();	installed += Install2<kFlatTree,   kRigid>();

	installed += Install1<kNiNode,     kTransform>();
	installed += Install1<kBSFadeNode, kTransform>();
	installed += Install1<kFlatTree,   kTransform>();

	installed += Install1<kNiNode,     kWorld>();
	installed += Install1<kBSFadeNode, kWorld>();
	installed += Install1<kFlatTree,   kWorld>();
	installed += Install1<kNiAVObject, kWorld>();

	_MESSAGE("hooks: %u of 16 pass-through hooks installed (they only count calls)", installed);
}

void SceneHooksSetTarget(unsigned root, unsigned flat)
{
	g_playerRoot = root;
	g_playerFlat = flat;
	_MESSAGE("hooks: counting the player's skeleton: root=%08X flat=%08X", root, flat);
}

void SceneHooksStartReport()
{
	HANDLE t = CreateThread(NULL, 0, ReportThread, NULL, 0, NULL);
	if(t) CloseHandle(t);
}

namespace
{
	// Posicao mundial do no raiz do jogador (translacao em +0x78).
	bool PlayerPos(float * p)
	{
		const unsigned root = g_playerRoot;
		if(!root || !ReadableMem(root + 0x78, 12)) return false;
		memcpy(p, (const void *)(root + 0x78), 12);
		return true;
	}

	DWORD WINAPI CycleThread(LPVOID)
	{
		_MESSAGE("pose test: waiting - WALK a few steps (about 1 m or more) and then STAND STILL for 3 s. It never starts during a loading screen.");

		const DWORD t0 = GetTickCount();
		float last[3] = { 0, 0, 0 };
		bool haveLast = false, announced = false;
		float walked = 0.0f;
		DWORD stillSince = GetTickCount();

		for(;;)
		{
			Sleep(100);
			if(GetTickCount() - t0 > 600000) { _MESSAGE("pose test: no walking detected for 10 min - giving up"); return 0; }

			float p[3];
			if(!PlayerPos(p)) continue;
			if(!haveLast) { memcpy(last, p, 12); haveLast = true; stillSince = GetTickCount(); continue; }

			const float step = sqrtf((p[0] - last[0]) * (p[0] - last[0]) + (p[1] - last[1]) * (p[1] - last[1]));
			if(step > 0.5f && step < 60.0f) walked += step;
			if(step > 1.0f) stillSince = GetTickCount();
			memcpy(last, p, 12);

			if(walked > 80.0f && !announced) { announced = true; _MESSAGE("pose test: walking detected - gameplay is live. Now stand still."); }
			if(announced && GetTickCount() - stillSince > 3000) break;
		}

		// 1) auto-verificacao da matematica (nao altera nada)
		g_fkCheckDone = 0;
		InterlockedExchange(&g_fkCheckRequested, 1);
		for(int w = 0; w < 100 && !g_fkCheckDone; ++w) Sleep(100);
		if(g_fkCheckDone)
		{
			_MESSAGE("pose test: self-check A (rebuilding the game's own world from effective locals): max error %.4f units, %.3f deg  -> maths OK if ~0",
					 (double)g_fkRecT, (double)g_fkRecRot);
			char idx[200] = ""; int n = 0;
			for(unsigned k = 0; k < g_fkBadCount && n >= 0; ++k) n += _snprintf_s(idx + n, sizeof(idx) - n, _TRUNCATE, " %u", g_fkBadIdx[k]);
			_MESSAGE("pose test: self-check B (hierarchy from direct local transforms): %u of %u entries differ from the game's world (max %.1f units, %.1f deg). First ones:%s",
					 g_fkMismatch, g_fkEntries, (double)g_fkMaxT, (double)g_fkMaxRotDeg, idx);
		}
		else
			_MESSAGE("pose test: self-check did not run (no pass seen)");

		// 2) o repouso autoral (referencia do deslocamento do centro do corpo)
		g_restReady = FC_GetPose(0, g_rest) ? 1 : 0;
		if(!g_restReady) { _MESSAGE("pose test: could not read the authored rest pose - stopping"); return 0; }

		// 3) movimentos reais, um de cada vez, na velocidade real: 4 s de pausa entre eles
		struct Item { int id; const char * name; };
		const Item items[] = { { 42, "contextMantle (climb over a ledge)" }, { 2, "up (climb a wall)" }, { 11, "hopUp" },
							   { 27, "kickUp" }, { 30, "flipUp (flip)" }, { 8, "reach" } };
		_MESSAGE("pose test: %d real FreeClimb motions will play one at a time, at real speed, 4 s apart. Watch the character from third person.",
				 (int)(sizeof(items) / sizeof(items[0])));

		for(size_t k = 0; k < sizeof(items) / sizeof(items[0]); ++k)
		{
			const float secs = FC_MotionSeconds(items[k].id);
			if(secs <= 0.0f) { _MESSAGE("pose test: motion %d '%s' not available - skipped", items[k].id, items[k].name); continue; }

			g_playMotion = items[k].id; g_playSeconds = secs; g_playFrames = 0; g_playStart = GetTickCount();
			InterlockedExchange(&g_playing, 1);
			_MESSAGE("pose test: PLAYING '%s' (%.2f s)", items[k].name, (double)secs);
			Sleep((DWORD)(secs * 1000.0f) + 400);
			InterlockedExchange(&g_playing, 0);
			_MESSAGE("pose test: finished '%s' - %ld frames written", items[k].name, g_playFrames);
			Sleep(4000);
		}
		_MESSAGE("pose test: all motions done");
		return 0;
	}
}

void SceneHooksBindBones(const int * entryIndex)
{
	unsigned bound = 0;
	for(int b = 0; b < 99; ++b)
	{
		g_boneEntry[b] = entryIndex[b];
		if(entryIndex[b] >= 0) ++bound;
	}
	g_restReady = FC_GetPose(0, g_rest) ? 1 : 0;
	_MESSAGE("pose engine: %u of 99 canonical bones are bound to flat-table entries; authored rest pose %s", bound, g_restReady ? "loaded" : "NOT available");
}

void SceneHooksStartPoseTest()
{
	static bool started = false;
	if(started) return;
	started = true;

	if(FC_PoseCount() == 0) { _MESSAGE("pose test: the animation library is not loaded - not started"); return; }

	HANDLE t = CreateThread(NULL, 0, CycleThread, NULL, 0, NULL);
	if(t) CloseHandle(t);
}

bool SceneHooksPlayMotion(int motion, float scaleY, float scaleZ, SceneFinishCallback onFinish)
{
	if(g_playing || !g_restReady || !g_playerFlat) return false;

	bool bound = false;
	for(int b = 0; b < 99; ++b) if(g_boneEntry[b] >= 0) bound = true;
	if(!bound) return false;

	const float secs = FC_MotionSeconds(motion);
	if(secs <= 0.0f) return false;

	g_playMotion = motion; g_playSeconds = secs; g_scaleY = scaleY; g_scaleZ = scaleZ; g_finishCb = onFinish;
	g_playFrames = 0; g_playStart = GetTickCount();
	InterlockedExchange(&g_playing, 1);
	return true;
}

bool SceneHooksIsPlaying()
{
	return g_playing != 0;
}

bool SceneHooksRootForward(float * x, float * y)
{
	const unsigned root = g_playerRoot;
	if(!root || !ReadableMem(root + 0x54, 36)) return false;

	const float * r = (const float *)(root + 0x54);		// rotacao mundial do no raiz, linhas
	const float fx = r[1], fy = r[4];						// coluna 1 = o eixo Y local (frente) no mundo
	const float len = sqrtf(fx * fx + fy * fy);
	if(len < 0.001f) return false;

	*x = fx / len; *y = fy / len;
	return true;
}

void SceneHooksSetTick(SceneTickCallback cb)
{
	g_tickCb = cb;
}

void SceneHooksSetManualPose(bool active, int motion, float phase, float scaleY, float scaleZ)
{
	g_manualMotion = motion;
	g_manualPhase = phase;
	g_scaleY = scaleY;
	g_scaleZ = scaleZ;
	InterlockedExchange(&g_manual, active ? 1 : 0);
}

// ----------------------------------------------------------------------------------------------
// Reencontrar o esqueleto do jogador. Chamar na thread principal (ao carregar numa tecla).
bool SceneHooksRefreshTarget()
{
	const unsigned kFlatVtable = 0x011186A4;						// BSFlattenedBoneTree

	const unsigned player = ReadableMem(0x01310588, 4) ? *(const unsigned *)0x01310588 : 0;
	if(!player || !ReadableMem(player, 4)) return false;
	const unsigned vtable = *(const unsigned *)player;
	if(!ReadableMem(vtable + 0x6F * 4, 4)) return false;

	typedef unsigned (__thiscall * GetRoot_t)(void *, unsigned);
	const GetRoot_t getRoot = (GetRoot_t)*(const unsigned *)(vtable + 0x6F * 4);
	const unsigned root = getRoot((void *)player, 0);
	if(!root || !ReadableMem(root, 0xC0)) return false;

	// procura a tabela achatada entre os descendentes da raiz (poucos niveis)
	unsigned flat = 0;
	unsigned level[3][64]; unsigned count[3] = { 1, 0, 0 };
	level[0][0] = root;
	for(int depth = 0; depth < 3 && !flat; ++depth)
	{
		for(unsigned i = 0; i < count[depth] && !flat; ++i)
		{
			const unsigned node = level[depth][i];
			if(!ReadableMem(node, 0xC0)) continue;
			if(*(const unsigned *)node == kFlatVtable) { flat = node; break; }
			if(depth == 2) continue;

			const unsigned data = *(const unsigned *)(node + 0xAC);
			const unsigned bound = *(const unsigned short *)(node + 0xB2);
			if(!data || bound == 0 || bound > 64 || !ReadableMem(data, bound * 4)) continue;
			for(unsigned c = 0; c < bound && count[depth + 1] < 64; ++c)
			{
				const unsigned child = *(const unsigned *)(data + 4 * c);
				if(child) level[depth + 1][count[depth + 1]++] = child;
			}
		}
	}
	if(!flat) return false;

	unsigned entries, n;
	if(!FlatArray(flat, &entries, &n)) return false;

	if(root == g_playerRoot && flat == g_playerFlat) return true;	// nada mudou

	// ligar os ossos canonicos as entradas, pelo nome (sem distinguir maiusculas)
	int map[99];
	const int engineOwned[] = { 0, 1, 2, 3, 42, 43, 60, 61, 62, 63, 64, 65, 66, 97, 98 };
	for(int bone = 0; bone < 99; ++bone)
	{
		map[bone] = -1;
		bool skip = false;
		for(size_t k = 0; k < sizeof(engineOwned) / sizeof(engineOwned[0]); ++k) if(engineOwned[k] == bone) skip = true;
		if(skip) continue;

		const char * name = FC_BoneName(bone);
		for(unsigned i = 0; i < n; ++i)
		{
			const unsigned e = entries + i * 0x78;
			if(!ReadableMem(e + 0x74, 4)) continue;
			const unsigned chars = *(const unsigned *)(e + 0x74);
			if(!chars || !ReadableMem(chars, 48)) continue;
			if(_strnicmp((const char *)chars, name, 47) == 0 && ((const char *)chars)[strlen(name)] == 0) { map[bone] = (int)i; break; }
		}
	}

	// trocar tudo de uma vez, sem a thread do esqueleto estar a meio de uma pose
	for(int spin = 0; spin < 200 && InterlockedCompareExchange(&g_inPose, 1, 0) != 0; ++spin) Sleep(1);
	unsigned bound = 0;
	for(int bone = 0; bone < 99; ++bone) { g_boneEntry[bone] = map[bone]; if(map[bone] >= 0) ++bound; }
	g_playerRoot = root;
	g_playerFlat = flat;
	InterlockedExchange(&g_inPose, 0);

	_MESSAGE("pose engine: skeleton target refreshed - root %08X, flat table %08X (%u entries), %u of 99 bones bound", root, flat, n, bound);
	return bound > 0;
}
