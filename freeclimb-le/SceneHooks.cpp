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

	// =============================================================================================
	// PASSO 25 - MOTOR DE POSE
	//
	// Como o FreeClimb original (SE) faz: a pose e escrita nas transformacoes LOCAIS dos ossos ANTES de o
	// motor correr cada passagem de atualizacao do esqueleto, e e o proprio motor que propaga o resultado
	// (nos, tabela achatada, pele, camara, armas...). Assim o corpo "obedece" aos ossos, em vez de se
	// repintar so a tabela achatada depois de o motor ter acabado (modo antigo, PoseMode=0).
	//
	// Onde esta o local de um osso: se a entrada achatada tem no (+0x70), o motor anima o NO (local em +0x20,
	// +0x44, +0x50); senao, a propria entrada (local em +0x00, +0x24, +0x30).
	// =============================================================================================
	SceneTickCallback	g_tickCb = NULL;
	volatile LONG		g_inPose = 0;
	int					g_boneEntry[99];
	unsigned			g_boneNode[99];				// o no de cada osso no momento da ligacao (0 = so entrada)
	float				g_rest[99 * 10];
	volatile LONG		g_restReady = 0;

	int					g_poseMode = 1;				// 1 = locais antes da passagem (novo); 0 = mundo depois da passagem (antigo)
	float				g_entryBlend = 0.14f, g_exitBlend = 0.22f;

	// pedido do jogo (escrito por SceneHooksSetPose) e a pose final deste quadro
	volatile LONG		g_reqActive = 0;
	float				g_reqPose[99 * 10];
	int					g_reqKey = 0;
	float				g_reqBlend = 0.15f;
	int					g_curKey = -1;
	bool				g_haveOut = false;
	float				g_out[99 * 10], g_xfFrom[99 * 10];
	float				g_xfT = 1.0f, g_xfDur = 0.15f, g_weightRaw = 0.0f;
	volatile LONG		g_frameActive = 0;
	float				g_frameWeight = 0.0f;
	float			g_folOut[99 * 10];
	float			g_folWeight = 0.0f;
	volatile LONG	g_folFrame = 0;
	LARGE_INTEGER		g_qpcFreq, g_qpcLast;
	bool				g_qpcReady = false;
	volatile LONG		g_fault = 0;

	struct Tr { float r[9]; float t[3]; float s; };		// NiTransform: rotacao 3x3 (linhas), translacao, escala

	// o que foi escrito por ultimo em cada osso e o que o motor tinha antes (para nao misturar a nossa propria escrita)
	Tr					g_written[99], g_native[99];
	bool				g_writtenValid[99];

	// estatisticas por tipo de passagem (para o log)
	volatile LONG		g_passApplied[kKinds];
	volatile LONG		g_passOverwritten[kKinds];
	volatile LONG		g_framesPosed = 0;
	DWORD				g_lastStatLog = 0;

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

	// ---- quaternios (x y z w); a matriz tem as COLUNAS como os eixos rodados (convencao do jogo)
	void FromQuat(const float * q, float * r)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		r[0] = 1 - 2 * (y * y + z * z);	r[1] = 2 * (x * y - z * w);		r[2] = 2 * (x * z + y * w);
		r[3] = 2 * (x * y + z * w);		r[4] = 1 - 2 * (x * x + z * z);	r[5] = 2 * (y * z - x * w);
		r[6] = 2 * (x * z - y * w);		r[7] = 2 * (y * z + x * w);		r[8] = 1 - 2 * (x * x + y * y);
	}

	void ToQuat(const float * r, float * q)
	{
		const float trace = r[0] + r[4] + r[8];
		if(trace > 0.0f)
		{
			const float s = sqrtf(trace + 1.0f) * 2.0f;
			q[3] = 0.25f * s; q[0] = (r[7] - r[5]) / s; q[1] = (r[2] - r[6]) / s; q[2] = (r[3] - r[1]) / s;
		}
		else if(r[0] > r[4] && r[0] > r[8])
		{
			const float s = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2.0f;
			q[3] = (r[7] - r[5]) / s; q[0] = 0.25f * s; q[1] = (r[1] + r[3]) / s; q[2] = (r[2] + r[6]) / s;
		}
		else if(r[4] > r[8])
		{
			const float s = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2.0f;
			q[3] = (r[2] - r[6]) / s; q[0] = (r[1] + r[3]) / s; q[1] = 0.25f * s; q[2] = (r[5] + r[7]) / s;
		}
		else
		{
			const float s = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2.0f;
			q[3] = (r[3] - r[1]) / s; q[0] = (r[2] + r[6]) / s; q[1] = (r[5] + r[7]) / s; q[2] = 0.25f * s;
		}
		const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		if(n > 1e-6f) { q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n; } else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
	}

	void QMul(const float * a, const float * b, float * o)
	{
		o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
		o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
		o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
		o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	}

	void QRotate(const float * q, const float * v, float * o)
	{
		const float ax = q[0], ay = q[1], az = q[2], w = q[3];
		const float tx = 2 * (ay * v[2] - az * v[1]), ty = 2 * (az * v[0] - ax * v[2]), tz = 2 * (ax * v[1] - ay * v[0]);
		o[0] = v[0] + tx * w + (ay * tz - az * ty);
		o[1] = v[1] + ty * w + (az * tx - ax * tz);
		o[2] = v[2] + tz * w + (ax * ty - ay * tx);
	}

	void QSlerp(const float * a, const float * b, float t, float * o)
	{
		float bb[4] = { b[0], b[1], b[2], b[3] };
		float d = a[0] * bb[0] + a[1] * bb[1] + a[2] * bb[2] + a[3] * bb[3];
		if(d < 0.0f) { bb[0] = -bb[0]; bb[1] = -bb[1]; bb[2] = -bb[2]; bb[3] = -bb[3]; d = -d; }
		float u = 1.0f - t, v = t;
		if(d < 0.9995f)
		{
			const float angle = acosf(d > 1.0f ? 1.0f : d), sn = sinf(angle);
			u = sinf((1.0f - t) * angle) / sn; v = sinf(t * angle) / sn;
		}
		o[0] = a[0] * u + bb[0] * v; o[1] = a[1] * u + bb[1] * v; o[2] = a[2] * u + bb[2] * v; o[3] = a[3] * u + bb[3] * v;
		const float n = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
		if(n > 1e-6f) { o[0] /= n; o[1] /= n; o[2] /= n; o[3] /= n; } else { o[0] = o[1] = o[2] = 0; o[3] = 1; }
	}

	float Ease(float t) { if(t < 0) t = 0; if(t > 1) t = 1; return t * t * (3.0f - 2.0f * t); }

	bool FinitePose(const float * p)
	{
		for(int i = 0; i < 99 * 10; ++i) if(!(p[i] == p[i]) || p[i] > 1e6f || p[i] < -1e6f) return false;
		return true;
	}

	// Mistura duas poses de 99 ossos (rotacao: slerp; translacao e escala: linear).
	void BlendPoses(const float * a, const float * b, float t, float * out)
	{
		for(int i = 0; i < 99; ++i)
		{
			const float * pa = a + i * 10, * pb = b + i * 10;
			float * po = out + i * 10;
			for(int k = 0; k < 3; ++k) po[k] = pa[k] + (pb[k] - pa[k]) * t;
			QSlerp(pa + 3, pb + 3, t, po + 3);
			for(int k = 7; k < 10; ++k) po[k] = pa[k] + (pb[k] - pa[k]) * t;
		}
	}

	// Um osso: o 'local' entra com o que o motor tem (nativo) e sai com o que queremos escrever.
	//  - todos os ossos: rotacao = mistura(nativo, pose) com o peso do quadro;
	//  - o centro do corpo (4): tambem recebe o deslocamento da pose e o ENQUADRAMENTO da raiz autoral (osso 0:
	//    e ai que o original poe a distancia a parede e a rotacao do corpo para a parede/corrida).
	void ComposeBoneP(int bone, Tr * local, const float * out, float w)
	{
		const float * p = out + bone * 10;
		float qn[4];
		ToQuat(local->r, qn);

		float q[4];
		QSlerp(qn, p + 3, w, q);

		if(bone != 4)
		{
			FromQuat(q, local->r);
			return;
		}

		float t[3];
		for(int k = 0; k < 3; ++k) t[k] = local->t[k] + (p[k] - g_rest[4 * 10 + k]) * w;

		const float * root = out;						// osso 0 = enquadramento do corpo
		const float identity[4] = { 0, 0, 0, 1 };
		float fq[4];
		QSlerp(identity, root + 3, w, fq);

		float rt[3], fx[3] = { t[0], t[1], t[2] };
		QRotate(fq, fx, rt);
		local->t[0] = rt[0] + root[0] * w; local->t[1] = rt[1] + root[1] * w; local->t[2] = rt[2] + root[2] * w;

		float qq[4];
		QMul(fq, q, qq);
		FromQuat(qq, local->r);
	}

	void ComposeBone(int bone, Tr * local) { ComposeBoneP(bone, local, g_out, g_frameWeight); }

	// Uma vez por quadro (na passagem "para baixo"): decide a pose final, com fusao de entrada, de saida e entre movimentos.
	void BeginFrame()
	{
		if(!g_qpcReady) { QueryPerformanceFrequency(&g_qpcFreq); QueryPerformanceCounter(&g_qpcLast); g_qpcReady = true; }
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		float dt = (float)((double)(now.QuadPart - g_qpcLast.QuadPart) / (double)g_qpcFreq.QuadPart);
		g_qpcLast = now;
		if(!(dt >= 0.0f)) dt = 0.0f;
		if(dt > 0.1f) dt = 0.1f;

		if(g_reqActive && g_restReady && FinitePose(g_reqPose))
		{
			if(g_curKey != g_reqKey)
			{
				if(g_haveOut && g_weightRaw > 0.05f && g_reqBlend > 0.001f)
				{
					memcpy(g_xfFrom, g_out, sizeof(g_out));
					g_xfT = 0.0f; g_xfDur = g_reqBlend;
				}
				else g_xfT = 1.0f;
				g_curKey = g_reqKey;
			}

			if(g_xfT < 1.0f)
			{
				g_xfT += dt / g_xfDur;
				if(g_xfT > 1.0f) g_xfT = 1.0f;
				BlendPoses(g_xfFrom, g_reqPose, Ease(g_xfT), g_out);
			}
			else memcpy(g_out, g_reqPose, sizeof(g_out));
			g_haveOut = true;

			g_weightRaw = g_entryBlend > 0.001f ? g_weightRaw + dt / g_entryBlend : 1.0f;
			if(g_weightRaw > 1.0f) g_weightRaw = 1.0f;
		}
		else
		{
			// sem pedido: a ultima pose desvanece-se para a do motor
			g_curKey = -1;
			if(g_haveOut)
			{
				g_weightRaw = g_exitBlend > 0.001f ? g_weightRaw - dt / g_exitBlend : 0.0f;
				if(g_weightRaw <= 0.0f) { g_weightRaw = 0.0f; g_haveOut = false; }
			}
			else g_weightRaw = 0.0f;
		}

		g_frameWeight = Ease(g_weightRaw);
		InterlockedExchange(&g_frameActive, (g_haveOut && g_frameWeight > 0.0f) ? 1 : 0);

		// os seguidores recebem uma copia da pose deste quadro (a pose e a do jogador)
		if(g_frameActive) { memcpy(g_folOut, g_out, sizeof(g_out)); g_folWeight = g_frameWeight; InterlockedExchange(&g_folFrame, 1); }
		else InterlockedExchange(&g_folFrame, 0);
	}

	// ---- modo novo: escrever os locais antes da passagem ----
	bool BindingStillValid(unsigned entries, unsigned count)
	{
		if(!ReadableMem(entries, count * 0x78)) return false;
		for(int b = 0; b < 99; ++b)
		{
			const int idx = g_boneEntry[b];
			if(idx < 0) continue;
			if((unsigned)idx >= count) return false;
			if(*(const unsigned *)(entries + (unsigned)idx * 0x78 + 0x70) != g_boneNode[b]) return false;
		}
		return true;
	}

	unsigned ApplyLocals(unsigned entries)
	{
		unsigned wrote = 0;
		for(int b = 0; b < 99; ++b)
		{
			const int idx = g_boneEntry[b];
			if(idx < 0) continue;

			const unsigned e = entries + (unsigned)idx * 0x78;
			const unsigned node = g_boneNode[b];
			float * rot = node ? (float *)(node + 0x20) : (float *)(e + 0x00);
			float * tr  = node ? (float *)(node + 0x44) : (float *)(e + 0x24);

			Tr cur;
			memcpy(cur.r, rot, 36); memcpy(cur.t, tr, 12); cur.s = 1.0f;

			// se o que la esta e o que escrevemos ha pouco (outra passagem do mesmo quadro), o "nativo" e o que guardamos
			const bool ours = g_writtenValid[b] && memcmp(cur.r, g_written[b].r, 36) == 0 && (b != 4 || memcmp(cur.t, g_written[b].t, 12) == 0);
			if(!ours) g_native[b] = cur;

			Tr local = g_native[b];
			ComposeBone(b, &local);

			memcpy(rot, local.r, 36);
			if(b == 4) memcpy(tr, local.t, 12);
			g_written[b] = local;
			if(b != 4) memcpy(g_written[b].t, cur.t, 12);
			g_writtenValid[b] = true;
			++wrote;
		}
		return wrote;
	}

	unsigned CountOverwritten(unsigned entries)
	{
		unsigned bad = 0;
		for(int b = 0; b < 99; ++b)
		{
			const int idx = g_boneEntry[b];
			if(idx < 0 || !g_writtenValid[b]) continue;
			const unsigned e = entries + (unsigned)idx * 0x78;
			const unsigned node = g_boneNode[b];
			const float * rot = node ? (const float *)(node + 0x20) : (const float *)(e + 0x00);
			if(memcmp(rot, g_written[b].r, 36) != 0) ++bad;
		}
		return bad;
	}

	// ---- seguidores: a pose do jogador aplicada ao esqueleto de outros atores (ate 3) ----
	struct FolTarget
	{
		unsigned		actor, root, flat;
		int				boneEntry[99];
		unsigned		boneNode[99];
		Tr				written[99], native[99];
		bool			writtenValid[99];
		volatile LONG	on, busy;
		bool			bound;
	};
	FolTarget		g_fol[3];

	int FollowerIndex(unsigned self)
	{
		for(int i = 0; i < 3; ++i) if(g_fol[i].bound && g_fol[i].flat == self) return i;
		return -1;
	}

	// Antes de uma passagem da tabela achatada de um seguidor: escreve-lhe os locais, como ao jogador.
	bool FollowerPrePass(int i, unsigned flat, unsigned * entriesOut)
	{
		FolTarget & f = g_fol[i];
		if(!f.on || !g_folFrame || g_folWeight <= 0.0f) return false;

		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count) || !ReadableMem(entries, count * 0x78)) return false;
		for(int b = 0; b < 99; ++b)
		{
			const int idx = f.boneEntry[b];
			if(idx < 0) continue;
			if((unsigned)idx >= count || *(const unsigned *)(entries + (unsigned)idx * 0x78 + 0x70) != f.boneNode[b]) { f.bound = false; return false; }
		}
		if(InterlockedCompareExchange(&f.busy, 1, 0) != 0) return false;

		float out[99 * 10];
		memcpy(out, g_folOut, sizeof(out));
		const float w = g_folWeight;
		for(int b = 0; b < 99; ++b)
		{
			const int idx = f.boneEntry[b];
			if(idx < 0) continue;

			const unsigned e = entries + (unsigned)idx * 0x78;
			const unsigned node = f.boneNode[b];
			float * rot = node ? (float *)(node + 0x20) : (float *)(e + 0x00);
			float * tr  = node ? (float *)(node + 0x44) : (float *)(e + 0x24);

			Tr cur;
			memcpy(cur.r, rot, 36); memcpy(cur.t, tr, 12); cur.s = 1.0f;
			const bool ours = f.writtenValid[b] && memcmp(cur.r, f.written[b].r, 36) == 0 && (b != 4 || memcmp(cur.t, f.written[b].t, 12) == 0);
			if(!ours) f.native[b] = cur;

			Tr local = f.native[b];
			ComposeBoneP(b, &local, out, w);

			memcpy(rot, local.r, 36);
			if(b == 4) memcpy(tr, local.t, 12);
			f.written[b] = local;
			if(b != 4) memcpy(f.written[b].t, cur.t, 12);
			f.writtenValid[b] = true;
		}
		InterlockedExchange(&f.busy, 0);
		*entriesOut = entries;
		return true;
	}

	void LogStats(unsigned flat)
	{
		const DWORD now = GetTickCount();
		if(now - g_lastStatLog < 1000) return;
		g_lastStatLog = now;
		unsigned bound = 0;
		for(int b = 0; b < 99; ++b) if(g_boneEntry[b] >= 0) ++bound;
		_MESSAGE("pose: mode %d, weight %.2f, %u bones bound | passes posed (down/sel/rigid/transform/world): %ld/%ld/%ld/%ld/%ld | overwritten by the engine after the pass: %ld/%ld/%ld/%ld/%ld",
				 g_poseMode, (double)g_frameWeight, bound,
				 g_passApplied[kDownward], g_passApplied[kSelected], g_passApplied[kRigid], g_passApplied[kTransform], g_passApplied[kWorld],
				 g_passOverwritten[kDownward], g_passOverwritten[kSelected], g_passOverwritten[kRigid], g_passOverwritten[kTransform], g_passOverwritten[kWorld]);
		for(int k = 0; k < kKinds; ++k) { g_passApplied[k] = 0; g_passOverwritten[k] = 0; }
	}

	// Antes de cada passagem da tabela achatada do jogador.
	bool PrePass(int kind, unsigned flat, unsigned * entriesOut)
	{
		if(kind == kDownward)
		{
			if(g_tickCb) g_tickCb();			// a logica de jogo decide a pose deste quadro
			BeginFrame();
		}
		if(!g_frameActive) return false;

		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count)) return false;
		if(!BindingStillValid(entries, count)) { InterlockedExchange(&g_fault, 1); return false; }
		if(InterlockedCompareExchange(&g_inPose, 1, 0) != 0) return false;

		ApplyLocals(entries);
		InterlockedExchange(&g_inPose, 0);
		InterlockedIncrement(&g_passApplied[kind]);
		if(kind == kDownward) InterlockedIncrement(&g_framesPosed);
		*entriesOut = entries;
		return true;
	}

	void PostPassAudit(int kind, unsigned flat, unsigned entries)
	{
		const unsigned bad = CountOverwritten(entries);
		if(bad) InterlockedExchangeAdd(&g_passOverwritten[kind], 1);
		if(kind == kDownward) LogStats(flat);
	}

	// ---- modo antigo (PoseMode=0): recalcula o mundo de toda a hierarquia DEPOIS da passagem e escreve-o ----
	Tr g_live[1024], g_world[1024];

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

	bool ComputeWorlds(unsigned flat, unsigned entries, unsigned count)
	{
		int boneOfEntry[1024];
		for(unsigned i = 0; i < count; ++i) boneOfEntry[i] = -1;
		for(int b = 0; b < 99; ++b)
			if(g_boneEntry[b] >= 0 && (unsigned)g_boneEntry[b] < count) boneOfEntry[g_boneEntry[b]] = b;

		Tr root;
		ReadWorld(flat, 0x54, 0x78, 0x84, &root);

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
			if(parent >= (int)i) return false;

			Tr inv, local;
			Inv(parent >= 0 ? g_live[parent] : root, &inv);
			Mul(inv, g_live[i], &local);					// local efetiva

			const int bone = boneOfEntry[i];
			if(bone >= 0) ComposeBone(bone, &local);
			Mul(parent >= 0 ? g_world[parent] : root, local, &g_world[i]);
		}
		return true;
	}

	void LegacyPostPass(unsigned flat)
	{
		if(!g_frameActive) return;
		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count)) return;
		if(InterlockedCompareExchange(&g_inPose, 1, 0) != 0) return;
		if(ComputeWorlds(flat, entries, count))
		{
			for(unsigned i = 0; i < count; ++i)
			{
				const unsigned e = entries + i * 0x78;
				WriteWorld(e, 0x34, 0x58, 0x64, g_world[i]);
				const unsigned node = *(const unsigned *)(e + 0x70);
				if(node && ReadableMem(node + 0x54, 0x34)) WriteWorld(node, 0x54, 0x78, 0x84, g_world[i]);
			}
			InterlockedIncrement(&g_framesPosed);
		}
		InterlockedExchange(&g_inPose, 0);
	}

	typedef void (__thiscall * Fn2)(void *, void *, unsigned);	// (this, dados, flags)  ret 8
	typedef void (__thiscall * Fn1)(void *, void *);			// (this, dados)         ret 4

	template<int T, int K> struct Pass2
	{
		static Fn2 original;
		static void __fastcall call(void * self, void *, void * data, unsigned flags)
		{
			Count(K, self);
			if(T == kFlatTree && g_poseMode == 1)
			{
				const int fi = FollowerIndex((unsigned)self);
				if(fi >= 0)
				{
					unsigned entries = 0;
					FollowerPrePass(fi, (unsigned)self, &entries);
					original(self, data, flags);
					return;
				}
			}
			const bool player = (T == kFlatTree) && ((unsigned)self == g_playerFlat) && g_playerFlat;
			if(player && g_poseMode == 1)
			{
				unsigned entries = 0;
				const bool posed = PrePass(K, (unsigned)self, &entries);
				original(self, data, flags);
				if(posed) PostPassAudit(K, (unsigned)self, entries);
				return;
			}

			original(self, data, flags);
			if(player && K == kDownward)
			{
				if(g_tickCb) g_tickCb();
				BeginFrame();
				LegacyPostPass((unsigned)self);
			}
		}
	};
	template<int T, int K> Fn2 Pass2<T, K>::original = NULL;

	template<int T, int K> struct Pass1
	{
		static Fn1 original;
		static void __fastcall call(void * self, void *, void * data)
		{
			Count(K, self);
			if(T == kFlatTree && K == kTransform && g_poseMode == 1)
			{
				const int fi = FollowerIndex((unsigned)self);
				if(fi >= 0)
				{
					unsigned entries = 0;
					FollowerPrePass(fi, (unsigned)self, &entries);
					original(self, data);
					return;
				}
			}
			const bool player = (T == kFlatTree) && (K == kTransform) && ((unsigned)self == g_playerFlat) && g_playerFlat;
			if(player && g_poseMode == 1)
			{
				unsigned entries = 0;
				const bool posed = PrePass(K, (unsigned)self, &entries);
				original(self, data);
				if(posed) PostPassAudit(K, (unsigned)self, entries);
				return;
			}
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
	// Atribui a cada osso canonico o no (ou so a entrada) de que e feito. Chamar com o bloqueio g_inPose.
	void FillBoneNodes(unsigned flat)
	{
		unsigned entries, count;
		for(int b = 0; b < 99; ++b) { g_boneNode[b] = 0; g_writtenValid[b] = false; }
		if(!flat || !FlatArray(flat, &entries, &count) || !ReadableMem(entries, count * 0x78)) return;

		for(int b = 0; b < 99; ++b)
		{
			const int idx = g_boneEntry[b];
			if(idx < 0 || (unsigned)idx >= count) { g_boneEntry[b] = -1; continue; }
			const unsigned node = *(const unsigned *)(entries + (unsigned)idx * 0x78 + 0x70);
			if(node && !ReadableMem(node, 0x58)) { g_boneEntry[b] = -1; continue; }	// no ilegivel: nao mexer
			g_boneNode[b] = node;
		}
	}
}

void SceneHooksConfigure(int poseMode, float entryBlend, float exitBlend)
{
	g_poseMode = (poseMode == 0) ? 0 : 1;
	g_entryBlend = entryBlend < 0.0f ? 0.0f : (entryBlend > 1.0f ? 1.0f : entryBlend);
	g_exitBlend = exitBlend < 0.0f ? 0.0f : (exitBlend > 1.5f ? 1.5f : exitBlend);
}

void SceneHooksBindBones(const int * entryIndex)
{
	for(int spin = 0; spin < 200 && InterlockedCompareExchange(&g_inPose, 1, 0) != 0; ++spin) Sleep(1);
	unsigned bound = 0;
	for(int b = 0; b < 99; ++b)
	{
		g_boneEntry[b] = entryIndex[b];
		if(entryIndex[b] >= 0) ++bound;
	}
	FillBoneNodes(g_playerFlat);
	InterlockedExchange(&g_inPose, 0);

	g_restReady = FC_GetPose(0, g_rest) ? 1 : 0;
	unsigned withNode = 0;
	for(int b = 0; b < 99; ++b) if(g_boneEntry[b] >= 0 && g_boneNode[b]) ++withNode;
	_MESSAGE("pose engine: %u of 99 canonical bones are bound to flat-table entries (%u of them have a scene node); authored rest pose %s",
			 bound, withNode, g_restReady ? "loaded" : "NOT available");
}

void SceneHooksSetPose(bool active, const float * pose990, int key, float blendSeconds)
{
	if(active && pose990)
	{
		memcpy(g_reqPose, pose990, sizeof(g_reqPose));
		g_reqKey = key;
		g_reqBlend = blendSeconds;
	}
	InterlockedExchange(&g_reqActive, (active && pose990) ? 1 : 0);
}

int SceneHooksBoundBones()
{
	int n = 0;
	for(int b = 0; b < 99; ++b) if(g_boneEntry[b] >= 0) ++n;
	return n;
}

bool SceneHooksIsPlaying()
{
	return g_frameActive != 0;
}

bool SceneHooksPoseFaulted()
{
	return InterlockedExchange(&g_fault, 0) != 0;
}

void SceneHooksStartPoseTest() {}

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

	if(root == g_playerRoot && flat == g_playerFlat)
	{
		// nada mudou na raiz; mas confirma que as ligacoes continuam a bater certo com as entradas
		if(BindingStillValid(entries, n)) return true;
		_MESSAGE("pose engine: the flat table entries changed under the same skeleton - binding again");
	}

	// liga os ossos canonicos as entradas, pelo nome (sem distinguir maiusculas)
	int map[99];
	const int engineOwned[] = { 0, 1, 2, 3, 33, 34, 42, 43, 60, 61, 62, 63, 64, 65, 66, 97, 98 };
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
	FillBoneNodes(flat);
	InterlockedExchange(&g_fault, 0);
	InterlockedExchange(&g_inPose, 0);

	_MESSAGE("pose engine: skeleton target refreshed - root %08X, flat table %08X (%u entries), %u of 99 bones bound", root, flat, n, bound);
	return bound > 0;
}

// ----------------------------------------------------------------------------------------------
// Seguidores: a mesma pose do jogador aplicada ao esqueleto de outros atores (ate 3).
bool SceneHooksBindFollower(int slot, void * actorPtr)
{
	if(slot < 0 || slot > 2 || !actorPtr) return false;
	const unsigned kFlatVtable = 0x011186A4;
	const unsigned actor = (unsigned)actorPtr;
	if(!ReadableMem(actor, 4)) return false;
	const unsigned vtable = *(const unsigned *)actor;
	if(!ReadableMem(vtable + 0x6F * 4, 4)) return false;

	typedef unsigned (__thiscall * GetRoot_t)(void *, unsigned);
	const GetRoot_t getRoot = (GetRoot_t)*(const unsigned *)(vtable + 0x6F * 4);
	const unsigned root = getRoot((void *)actor, 0);
	if(!root || !ReadableMem(root, 0xC0)) return false;

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
	if(!flat || flat == g_playerFlat) return false;

	unsigned entries, n;
	if(!FlatArray(flat, &entries, &n) || !ReadableMem(entries, n * 0x78)) return false;

	int map[99];
	const int engineOwned[] = { 0, 1, 2, 3, 33, 34, 42, 43, 60, 61, 62, 63, 64, 65, 66, 97, 98 };
	unsigned bound = 0;
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
			if(_strnicmp((const char *)chars, name, 47) == 0 && ((const char *)chars)[strlen(name)] == 0) { map[bone] = (int)i; ++bound; break; }
		}
	}
	if(bound < 40) { _MESSAGE("follower %d: only %u bones matched - not binding", slot, bound); return false; }

	FolTarget & f = g_fol[slot];
	for(int spin = 0; spin < 200 && InterlockedCompareExchange(&f.busy, 1, 0) != 0; ++spin) Sleep(1);
	f.on = 0;
	f.bound = false;
	for(int b = 0; b < 99; ++b)
	{
		f.boneEntry[b] = map[b]; f.boneNode[b] = 0; f.writtenValid[b] = false;
		if(map[b] < 0) continue;
		const unsigned node = *(const unsigned *)(entries + (unsigned)map[b] * 0x78 + 0x70);
		if(node && !ReadableMem(node, 0x58)) { f.boneEntry[b] = -1; continue; }
		f.boneNode[b] = node;
	}
	f.actor = actor; f.root = root; f.flat = flat;
	f.bound = true;
	InterlockedExchange(&f.busy, 0);

	_MESSAGE("follower %d: skeleton bound - root %08X, flat table %08X, %u of 99 bones", slot, root, flat, bound);
	return true;
}

void SceneHooksUnbindFollower(int slot)
{
	if(slot < 0 || slot > 2) return;
	FolTarget & f = g_fol[slot];
	for(int spin = 0; spin < 200 && InterlockedCompareExchange(&f.busy, 1, 0) != 0; ++spin) Sleep(1);
	f.on = 0; f.bound = false; f.flat = 0; f.actor = 0;
	InterlockedExchange(&f.busy, 0);
}

void SceneHooksFollowerPose(int slot, bool on)
{
	if(slot < 0 || slot > 2) return;
	if(!on) for(int b = 0; b < 99; ++b) g_fol[slot].writtenValid[b] = false;
	g_fol[slot].on = on ? 1 : 0;
}

bool SceneHooksFollowerBound(int slot)
{
	return slot >= 0 && slot <= 2 && g_fol[slot].bound;
}
