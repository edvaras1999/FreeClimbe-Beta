// Gameplay.cpp - passo 20: ESCALAR NA PAREDE.
//   Salto junto a uma parede  -> o personagem agarra-se a parede (pose de pendurado).
//   Frente / Tras             -> sobe / desce (animacoes "up" e "down", o personagem move-se mesmo).
//   Esquerda / Direita        -> anda de lado (animacoes "left" e "right").
//   Chegar ao topo            -> passa por cima da borda (animacao "contextMantle") e fica em cima.
//   Salto ou Agachar          -> larga a parede.  Chegar ao chao a descer -> larga.
// Reutiliza o codigo verificado do Freerun LE (Engine.h, Havok.h) e o motor de poses do FreeClimb (SceneHooks).
// As afinacoes ficam em Data\SKSE\Plugins\FreeClimbLE.ini e sao relidas de cada vez que o personagem se agarra.

#include <math.h>
#include <string.h>
#include <windows.h>

#include <string>
#include <vector>

#include "Engine.h"
#include "Gameplay.h"
#include "Havok.h"
#include "Log.h"
#include "PackLoader.h"
#include "SceneHooks.h"

#include "skse/GameInput.h"
#include "skse/GameReferences.h"

namespace
{
	// ------------------------------------------------------------------ afinacoes (FreeClimbLE.ini)
	struct Config
	{
		float	reach;			// ate onde procura a parede (unidades)
		float	gap;			// distancia do personagem a parede enquanto agarrado
		float	climbSpeed, downSpeed, sideSpeed;	// velocidades na parede (unidades por segundo)
		float	minLedge, maxLedge;					// bordas a passar por cima (altura acima dos pes)
		float	headHeight;		// altura da cabeca: onde se procura o topo da parede
		int		alwaysCling;	// 1 = o salto junto a uma parede agarra-se sempre; 0 = passa por cima se for baixa
		int		faceWall;		// 1 = vira o personagem de frente para a parede enquanto agarrado
		float	hopScale;		// multiplica a distancia dos saltos na parede (1 = a da animacao)
		float	hopSpeed;		// multiplica a velocidade dos saltos (1 = a duracao da animacao)
	};
	Config g_cfg;

	void DefaultConfig()
	{
		g_cfg.reach = 110.0f;	g_cfg.gap = 30.0f;
		g_cfg.climbSpeed = 100.0f;	g_cfg.downSpeed = 78.0f;	g_cfg.sideSpeed = 82.0f;		// os padroes do FreeClimb original
		g_cfg.minLedge = 80.0f;	g_cfg.maxLedge = 180.0f;
		g_cfg.headHeight = 125.0f;
		g_cfg.alwaysCling = 1;
		g_cfg.faceWall = 1;
		g_cfg.hopScale = 1.4f;	g_cfg.hopSpeed = 1.3f;
	}

	bool ReadWholeFile(const char * path, std::string * out)
	{
		HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(file == INVALID_HANDLE_VALUE) return false;

		char buffer[4096];
		DWORD got = 0;
		while(ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got > 0) out->append(buffer, got);
		CloseHandle(file);
		return true;
	}

	bool KeyIs(const std::string & key, const char * name)
	{
		return _stricmp(key.c_str(), name) == 0;
	}

	void LoadConfig()
	{
		DefaultConfig();

		std::string text;
		if(!ReadWholeFile("Data\\SKSE\\Plugins\\FreeClimbLE.ini", &text))
		{
			_MESSAGE("gameplay: no FreeClimbLE.ini found - using the defaults");
			return;
		}

		size_t pos = 0;
		while(pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if(end == std::string::npos) end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;

			while(!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ' || line[line.size() - 1] == '\t')) line.erase(line.size() - 1);
			size_t start = line.find_first_not_of(" \t");
			if(start == std::string::npos) continue;
			line = line.substr(start);
			if(line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

			const size_t eq = line.find('=');
			if(eq == std::string::npos) continue;

			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			while(!key.empty() && (key[key.size() - 1] == ' ' || key[key.size() - 1] == '\t')) key.erase(key.size() - 1);
			const size_t vs = value.find_first_not_of(" \t");
			value = vs == std::string::npos ? "" : value.substr(vs);
			const float f = (float)atof(value.c_str());

			if(KeyIs(key, "Reach"))			g_cfg.reach = f;
			else if(KeyIs(key, "WallGap"))		g_cfg.gap = f;
			else if(KeyIs(key, "ClimbSpeed"))	g_cfg.climbSpeed = f;
			else if(KeyIs(key, "DownSpeed"))	g_cfg.downSpeed = f;
			else if(KeyIs(key, "SideSpeed"))	g_cfg.sideSpeed = f;
			else if(KeyIs(key, "MinLedge"))		g_cfg.minLedge = f;
			else if(KeyIs(key, "MaxLedge"))		g_cfg.maxLedge = f;
			else if(KeyIs(key, "HeadHeight"))	g_cfg.headHeight = f;
			else if(KeyIs(key, "AlwaysCling"))	g_cfg.alwaysCling = atoi(value.c_str());
			else if(KeyIs(key, "FaceWall"))		g_cfg.faceWall = atoi(value.c_str());
			else if(KeyIs(key, "HopScale"))		g_cfg.hopScale = f;
			else if(KeyIs(key, "HopSpeed"))		g_cfg.hopSpeed = f;
		}

		_MESSAGE("gameplay: settings: reach %.0f, wall gap %.0f, speeds up/down/side %.0f/%.0f/%.0f, ledge %.0f-%.0f, head %.0f, alwaysCling %d, faceWall %d, hop scale/speed %.2f/%.2f",
				 (double)g_cfg.reach, (double)g_cfg.gap, (double)g_cfg.climbSpeed, (double)g_cfg.downSpeed, (double)g_cfg.sideSpeed,
				 (double)g_cfg.minLedge, (double)g_cfg.maxLedge, (double)g_cfg.headHeight, g_cfg.alwaysCling, g_cfg.faceWall, (double)g_cfg.hopScale, (double)g_cfg.hopSpeed);
	}

	// ------------------------------------------------------------------ estado
	enum State { kIdle = 0, kCling = 1, kTopOut = 2, kHop = 3 };

	// os movimentos do pacote do FreeClimb
	const int	kHang = 1, kUp = 2, kDown = 3, kLeft = 4, kRight = 5, kMantle = 42;
	const float	kUpNatural = 90.3f / 2.0f;			// velocidade natural da animacao "up"/"down" (unidades/s)
	const float	kSideNatural = 42.3f / 1.17f;		// idem "left"/"right"
	const float	kMantleForward = 62.9f, kMantleUp = 132.3f;
	const int	kHopLeft = 9, kHopRight = 10, kHopUp = 11;
	const float	kHopLeftDist = -105.3f, kHopRightDist = 93.7f, kHopUpDist = 55.1f;		// deslocamento das animacoes

	volatile LONG	g_state = kIdle;
	DWORD			g_lastTick = 0, g_attachTime = 0;
	float			g_pos[3];			// pes do personagem enquanto agarrado
	float			g_n[2];				// normal da parede (aponta da parede para o personagem)
	float			g_phase = 0.0f;
	int				g_motion = kHang;
	float			g_destX = 0, g_destY = 0, g_destZ = 0;
	DWORD			g_mantleStart = 0, g_hopStart = 0;
	int				g_hopMotion = 0;
	float			g_hopSecs = 1.0f, g_hopFrom[3], g_hopTo[3];
	float			g_mantleStartPos[3], g_mantleDir[2], g_mantleScaleY = 1.0f, g_mantleScaleZ = 1.0f, g_mantleSecs = 1.87f;
	float			g_rest[99 * 10], g_buf[99 * 10];
	bool			g_restOk = false;

	struct InputState { volatile LONG forward, back, left, right, jumpEdge, sneakEdge; };
	InputState g_in;

	NiPoint3 Add(const NiPoint3 & a, const NiPoint3 & b)	{ return NiPoint3(a.x + b.x, a.y + b.y, a.z + b.z); }

	template<class Fn> void PatchSlot(UInt32 * vtable, int slot, void * hook, Fn * original)
	{
		volatile LONG * entry = (volatile LONG *)(vtable + slot);
		*original = (Fn)*entry;

		DWORD old = 0;
		if(!VirtualProtect((LPVOID)entry, 4, PAGE_READWRITE, &old)) return;
		InterlockedExchange(entry, (LONG)hook);
		VirtualProtect((LPVOID)entry, 4, old, &old);
	}

	// ------------------------------------------------------------------ o mundo: personagem, controlador, raios
	Actor * ThePlayer()		{ return (Actor *)Engine::Player(); }

	void PlaceController(void * ctrl, float x, float y, float z)
	{
		const float scale = Engine::HavokScale();
		__declspec(align(16)) float target[4];
		target[0] = x * scale; target[1] = y * scale; target[2] = z * scale; target[3] = 0.0f;
		Engine::ControllerSetPosition(ctrl, target, true, true);

		__declspec(align(16)) float stop[4] = { 0, 0, 0, 0 };
		Engine::ControllerSetLinearVelocity(ctrl, stop);			// sem gravidade nem inercia enquanto agarrado
	}

	struct WallHit { bool found; float dist, x, y, z, nx, ny; };

	// A parede a frente: tres raios a alturas diferentes; fica o mais proximo que seja uma superficie vertical virada para nos.
	WallHit ProbeWall(Actor * player, float fx, float fy, float px, float py, float pz, float maxDist, const float * heights, int count)
	{
		WallHit best;
		memset(&best, 0, sizeof(best));
		best.dist = 1e9f;

		for(int k = 0; k < count; ++k)
		{
			const NiPoint3 start(px, py, pz + heights[k]);
			const Havok::Result r = Havok::RayCast(start, NiPoint3(fx, fy, 0.0f), maxDist, Havok::kLayer_Transparent, player);
			if(!r.didHit) continue;
			if(fabsf(r.normal.z) > 0.45f) continue;										// nao e uma parede
			if(r.normal.x * fx + r.normal.y * fy > -0.3f) continue;						// nao esta virada para nos
			if(r.distance < best.dist)
			{
				best.found = true;
				best.dist = r.distance;
				best.x = r.hitPosition.x; best.y = r.hitPosition.y; best.z = r.hitPosition.z;
				const float len = sqrtf(r.normal.x * r.normal.x + r.normal.y * r.normal.y);
				if(len > 0.01f) { best.nx = r.normal.x / len; best.ny = r.normal.y / len; }
				else best.found = false;
			}
		}
		return best;
	}

	struct Ledge { float wallDistance, height, topZ; };

	// O topo de uma borda a frente de (px,py,pz) na direcao (fx,fy): parede, superficie plana por cima e espaco para a cabeca.
	bool FindLedge(Actor * player, float fx, float fy, float px, float py, float pz, Ledge * out, const char ** reason)
	{
		static const float heights[3] = { 50.0f, 95.0f, 140.0f };
		const WallHit wall = ProbeWall(player, fx, fy, px, py, pz, g_cfg.reach + 10.0f, heights, 3);
		if(!wall.found) { *reason = "no wall ahead"; return false; }

		const float reach = wall.dist + 25.0f;
		const NiPoint3 above(px + fx * reach, py + fy * reach, pz + 260.0f);
		const Havok::Result top = Havok::RayCast(above, NiPoint3(0, 0, -1), 260.0f, Havok::kLayer_Transparent, player);
		if(!top.didHit || top.normal.z < 0.7f) { *reason = "no flat top behind the wall"; return false; }

		const float height = top.hitPosition.z - pz;
		if(height < g_cfg.minLedge) { *reason = "ledge too low"; return false; }
		if(height > g_cfg.maxLedge) { *reason = "ledge too high"; return false; }

		const Havok::Result clear = Havok::RayCast(Add(top.hitPosition, NiPoint3(0, 0, 8.0f)), NiPoint3(0, 0, 1), 110.0f, Havok::kLayer_Transparent, player);
		if(clear.didHit && clear.distance < 100.0f) { *reason = "no head room on top"; return false; }

		out->wallDistance = wall.dist;
		out->height = height;
		out->topZ = top.hitPosition.z;
		return true;
	}

	// ------------------------------------------------------------------ passar por cima da borda
	// O personagem MOVE-SE mesmo ao longo do caminho da animacao (a camara e as armas acompanham-no) e, no fim,
	// ja esta em cima da borda: nao ha "salto" de posicao. O desenho do corpo usa so as rotacoes do clipe.
	void FinishMantle()
	{
		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(ctrl) PlaceController(ctrl, g_destX, g_destY, g_destZ + 1.0f);
		SceneHooksSetManualPose(false, kHang, 0, 1, 1);
		InterlockedExchange(&g_state, kIdle);
		_MESSAGE("gameplay: climb finished - player on top at (%.0f %.0f %.0f)", (double)g_destX, (double)g_destY, (double)g_destZ);
	}

	void TickMantle(void * ctrl, DWORD now)
	{
		const float phase = (float)(now - g_mantleStart) / 1000.0f / g_mantleSecs;
		if(phase >= 1.0f || !FC_SampleMotion(kMantle, phase, g_buf)) { FinishMantle(); return; }

		// o deslocamento do centro do corpo no clipe, ajustado a borda real, passa a ser o movimento do personagem
		const float dy = (g_buf[4 * 10 + 1] - g_rest[4 * 10 + 1]) * g_mantleScaleY;
		const float dz = (g_buf[4 * 10 + 2] - g_rest[4 * 10 + 2]) * g_mantleScaleZ;
		PlaceController(ctrl, g_mantleStartPos[0] + g_mantleDir[0] * dy, g_mantleStartPos[1] + g_mantleDir[1] * dy, g_mantleStartPos[2] + dz);
		SceneHooksSetManualPose(true, kMantle, phase, 0.0f, 0.0f);			// escalas 0: so as rotacoes, sem deslocar o corpo
	}

	// Comeca a passar por cima da borda a partir do ponto (px,py,pz), virado para (fx,fy).
	bool StartMantle(Actor * player, float fx, float fy, float px, float py, float pz, const char ** reason)
	{
		Ledge ledge;
		if(!FindLedge(player, fx, fy, px, py, pz, &ledge, reason)) return false;

		if(!g_restOk) g_restOk = FC_GetPose(0, g_rest);
		const float secs = FC_MotionSeconds(kMantle);
		if(!g_restOk || secs <= 0.0f) { *reason = "mantle animation not available"; return false; }

		const float forward = ledge.wallDistance + 40.0f;
		g_mantleScaleZ = ledge.height / kMantleUp;
		g_mantleScaleY = forward / kMantleForward;
		g_mantleSecs = secs;
		g_mantleStartPos[0] = px; g_mantleStartPos[1] = py; g_mantleStartPos[2] = pz;
		g_mantleDir[0] = fx; g_mantleDir[1] = fy;
		g_destX = px + fx * forward;
		g_destY = py + fy * forward;
		g_destZ = ledge.topZ;
		g_mantleStart = g_lastTick = GetTickCount();

		SceneHooksSetManualPose(true, kMantle, 0.0f, 0.0f, 0.0f);
		InterlockedExchange(&g_state, kTopOut);
		_MESSAGE("gameplay: MANTLE over a ledge %.0f units above the feet (wall %.0f ahead)", (double)ledge.height, (double)ledge.wallDistance);
		return true;
	}

	// ------------------------------------------------------------------ agarrar-se a parede
	void Release(const char * why, float pushSpeed = 60.0f)
	{
		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(ctrl)
		{
			// um pequeno empurrao para longe da parede, para nao ficar encostado a geometria ao cair
			const float v = ((g_state == kCling || g_state == kHop) ? pushSpeed : 0.0f) * Engine::HavokScale();
			__declspec(align(16)) float push[4] = { g_n[0] * v, g_n[1] * v, 0.0f, 0.0f };
			Engine::ControllerSetLinearVelocity(ctrl, push);
		}
		SceneHooksSetManualPose(false, kHang, 0, 1, 1);
		InterlockedExchange(&g_state, kIdle);
		_MESSAGE("gameplay: released from the wall (%s)", why);
	}

	bool StartCling(Actor * player, void * ctrl, float fx, float fy)
	{
		const NiPoint3 feet = Engine::Position(player);
		static const float heights[3] = { 55.0f, 95.0f, 135.0f };
		const WallHit wall = ProbeWall(player, fx, fy, feet.x, feet.y, feet.z, g_cfg.reach, heights, 3);
		if(!wall.found) return false;

		g_n[0] = wall.nx; g_n[1] = wall.ny;
		g_pos[0] = wall.x + g_n[0] * g_cfg.gap;
		g_pos[1] = wall.y + g_n[1] * g_cfg.gap;
		g_pos[2] = feet.z;

		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
		if(g_cfg.faceWall) Engine::Rotation(player).z = atan2f(-g_n[0], -g_n[1]);

		g_phase = 0.0f; g_motion = kHang;
		g_attachTime = g_lastTick = GetTickCount();
		g_in.jumpEdge = 0; g_in.sneakEdge = 0;
		SceneHooksSetManualPose(true, kHang, 0.0f, 1.0f, 1.0f);
		InterlockedExchange(&g_state, kCling);

		_MESSAGE("gameplay: CLINGING to the wall (%.0f units ahead). W/S climb, A/D sidestep, Jump or Sneak lets go.", (double)wall.dist);
		return true;
	}

	// ------------------------------------------------------------------ saltos na parede (movimento + salto)
	void StartHop(int motion, float lateral, float vertical)
	{
		const float secs = FC_MotionSeconds(motion);
		if(secs <= 0.0f) { Release("hop animation not available"); return; }

		const float rx = -g_n[1], ry = g_n[0];									// a direita ao longo da parede
		g_hopFrom[0] = g_pos[0]; g_hopFrom[1] = g_pos[1]; g_hopFrom[2] = g_pos[2];
		g_hopTo[0] = g_pos[0] + rx * lateral * g_cfg.hopScale;
		g_hopTo[1] = g_pos[1] + ry * lateral * g_cfg.hopScale;
		g_hopTo[2] = g_pos[2] + vertical * g_cfg.hopScale;
		g_hopMotion = motion;
		g_hopSecs = secs / (g_cfg.hopSpeed > 0.1f ? g_cfg.hopSpeed : 1.0f);
		g_hopStart = g_lastTick = GetTickCount();

		SceneHooksSetManualPose(true, motion, 0.0f, 1.0f, 1.0f);
		InterlockedExchange(&g_state, kHop);
		_MESSAGE("gameplay: HOP (%s) %.0f units", motion == kHopUp ? "up" : (lateral < 0.0f ? "left" : "right"),
				 (double)(fabsf(lateral) + fabsf(vertical)) * g_cfg.hopScale);
	}

	void TickHop(Actor * player, void * ctrl, DWORD now)
	{
		float p = (float)(now - g_hopStart) / 1000.0f / g_hopSecs;
		if(p > 1.0f) p = 1.0f;
		const float e = p * p * (3.0f - 2.0f * p);								// suave no inicio e no fim

		for(int k = 0; k < 3; ++k) g_pos[k] = g_hopFrom[k] + (g_hopTo[k] - g_hopFrom[k]) * e;
		if(g_cfg.faceWall) Engine::Rotation(player).z = atan2f(-g_n[0], -g_n[1]);
		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
		SceneHooksSetManualPose(true, g_hopMotion, p, 1.0f, 1.0f);
		if(p < 1.0f) return;

		// aterragem: ha parede aqui? Se sim, volta a agarrar-se; se nao, o salto falhou e cai.
		static const float chest[1] = { 70.0f };
		const WallHit w = ProbeWall(player, -g_n[0], -g_n[1], g_pos[0], g_pos[1], g_pos[2], g_cfg.gap + 50.0f, chest, 1);
		if(!w.found) { Release("the hop missed the wall"); return; }

		g_n[0] = w.nx; g_n[1] = w.ny;
		g_pos[0] = w.x + g_n[0] * g_cfg.gap;
		g_pos[1] = w.y + g_n[1] * g_cfg.gap;
		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);

		g_phase = 0.0f; g_motion = kHang;
		g_attachTime = g_lastTick = now;
		g_in.jumpEdge = 0; g_in.sneakEdge = 0;
		SceneHooksSetManualPose(true, kHang, 0.0f, 1.0f, 1.0f);
		InterlockedExchange(&g_state, kCling);
		_MESSAGE("gameplay: hop landed - clinging again");
	}

	// A cada atualizacao do esqueleto do jogador (thread de trabalho do jogo).
	void OnTick()
	{
		if(g_state == kIdle) return;

		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(!ctrl) { Release("no controller"); return; }

		const DWORD now = GetTickCount();
		if(g_state == kTopOut) { g_lastTick = now; TickMantle(ctrl, now); return; }
		if(g_state == kHop) { g_lastTick = now; TickHop(player, ctrl, now); return; }

		float dt = (float)(now - g_lastTick) / 1000.0f;
		if(dt > 0.1f) dt = 0.1f;
		g_lastTick = now;

		// soltar: salto ou agachar (ignora a pressao que acabou de nos agarrar)
		const LONG jump = InterlockedExchange(&g_in.jumpEdge, 0), sneak = InterlockedExchange(&g_in.sneakEdge, 0);
		const bool up = g_in.forward && !g_in.back, down = g_in.back && !g_in.forward;
		const bool left = g_in.left && !g_in.right, right = g_in.right && !g_in.left;

		if(now - g_attachTime > 400 && (jump || sneak))
		{
			if(sneak && !jump)	{ Release("sneak"); return; }
			// Salto com direcao: salta nessa direcao. S + Salto: empurra-se para longe. Salto sozinho: larga no sitio.
			if(up)			{ StartHop(kHopUp, 0.0f, kHopUpDist); return; }
			if(left)		{ StartHop(kHopLeft, kHopLeftDist, 0.0f); return; }
			if(right)		{ StartHop(kHopRight, kHopRightDist, 0.0f); return; }
			if(down)		{ Release("pushed away from the wall", 170.0f); return; }
			Release("let go in place", 0.0f);
			return;
		}

		float vz = 0.0f, vt = 0.0f, rate = 0.0f;
		int motion = kHang;
		if(up)			{ motion = kUp;		vz = g_cfg.climbSpeed;		rate = g_cfg.climbSpeed / kUpNatural; }
		else if(down)	{ motion = kDown;	vz = -g_cfg.downSpeed;		rate = g_cfg.downSpeed / kUpNatural; }
		else if(left)	{ motion = kLeft;	vt = -g_cfg.sideSpeed;		rate = g_cfg.sideSpeed / kSideNatural; }
		else if(right)	{ motion = kRight;	vt = g_cfg.sideSpeed;		rate = g_cfg.sideSpeed / kSideNatural; }

		if(motion != g_motion) { g_motion = motion; g_phase = 0.0f; }
		if(rate > 0.0f)
		{
			const float secs = FC_MotionSeconds(motion);
			if(secs > 0.0f) { g_phase += dt * rate / secs; while(g_phase >= 1.0f) g_phase -= 1.0f; }
		}
		else g_phase = 0.0f;

		// canto INTERIOR: ha outra parede mesmo ao lado, no sentido do movimento? Passa para ela (em vez de entrar nela).
		if(vt != 0.0f)
		{
			const float tx = (vt > 0.0f) ? -g_n[1] : g_n[1], ty = (vt > 0.0f) ? g_n[0] : -g_n[0];
			static const float chestH[1] = { 70.0f };
			const WallHit side = ProbeWall(player, tx, ty, g_pos[0], g_pos[1], g_pos[2], fabsf(vt) * dt + 18.0f, chestH, 1);
			if(side.found)
			{
				g_n[0] = side.nx; g_n[1] = side.ny;
				g_pos[0] = side.x + g_n[0] * g_cfg.gap;
				g_pos[1] = side.y + g_n[1] * g_cfg.gap;
				vt = 0.0f;
				_MESSAGE("gameplay: inner corner - switched to the next wall");
			}
		}

		// onde fica o personagem agora: sobe/desce em z e anda de lado ao longo da parede (a direita = (-ny, nx))
		g_pos[0] += (-g_n[1]) * vt * dt;
		g_pos[1] += ( g_n[0]) * vt * dt;
		g_pos[2] += vz * dt;

		// seguir a parede: um raio ao peito e outro a cabeca, na direcao da parede (-normal)
		const float fx = -g_n[0], fy = -g_n[1];
		static const float chest[1] = { 70.0f };
		const WallHit w = ProbeWall(player, fx, fy, g_pos[0], g_pos[1], g_pos[2], g_cfg.gap + 40.0f, chest, 1);
		if(!w.found)
		{
			if(vt != 0.0f)
			{
				// canto EXTERIOR (por exemplo, a esquina de um pilar): procura a face seguinte, por tras da esquina
				const float tx = (vt > 0.0f) ? -g_n[1] : g_n[1], ty = (vt > 0.0f) ? g_n[0] : -g_n[0];
				const float off = g_cfg.gap + 20.0f;
				static const float chestH[1] = { 70.0f };
				const WallHit around = ProbeWall(player, -tx, -ty, g_pos[0] - g_n[0] * off, g_pos[1] - g_n[1] * off, g_pos[2], g_cfg.gap + 70.0f, chestH, 1);
				if(around.found)
				{
					g_n[0] = around.nx; g_n[1] = around.ny;
					g_pos[0] = around.x + g_n[0] * g_cfg.gap;
					g_pos[1] = around.y + g_n[1] * g_cfg.gap;
					if(g_cfg.faceWall) Engine::Rotation(player).z = atan2f(-g_n[0], -g_n[1]);
					PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
					SceneHooksSetManualPose(true, g_motion, g_phase, 1.0f, 1.0f);
					_MESSAGE("gameplay: outer corner - wrapped around to the next face");
					return;
				}
			}
			if(up)
			{
				const char * why = "";
				if(StartMantle(player, fx, fy, g_pos[0], g_pos[1], g_pos[2], &why)) return;
			}
			Release("the wall ended");
			return;
		}
		g_n[0] = w.nx; g_n[1] = w.ny;
		g_pos[0] = w.x + g_n[0] * g_cfg.gap;
		g_pos[1] = w.y + g_n[1] * g_cfg.gap;

		// chegou ao topo? (a subir, sem parede a altura da cabeca) -> passar por cima da borda
		if(up)
		{
			const float head[1] = { g_cfg.headHeight + 15.0f };
			const WallHit h = ProbeWall(player, -g_n[0], -g_n[1], g_pos[0], g_pos[1], g_pos[2], g_cfg.gap + 45.0f, head, 1);
			if(!h.found)
			{
				const char * why = "";
				if(StartMantle(player, -g_n[0], -g_n[1], g_pos[0], g_pos[1], g_pos[2], &why)) return;
				// sem borda alcancavel: nao sobe mais, fica agarrado
				g_pos[2] -= vz * dt;
				vz = 0.0f;
				g_motion = kHang; g_phase = 0.0f; rate = 0.0f;
			}
		}

		// chegou ao chao a descer?
		if(down)
		{
			const Havok::Result ground = Havok::RayCast(NiPoint3(g_pos[0], g_pos[1], g_pos[2] + 20.0f), NiPoint3(0, 0, -1), 40.0f, Havok::kLayer_Transparent, player);
			if(ground.didHit && ground.normal.z > 0.6f) { Release("reached the ground"); return; }
		}

		if(g_cfg.faceWall) Engine::Rotation(player).z = atan2f(-g_n[0], -g_n[1]);
		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
		SceneHooksSetManualPose(true, g_motion, g_phase, 1.0f, 1.0f);
	}

	// ------------------------------------------------------------------ teclas
	typedef bool (__thiscall * CanProcess_t)(void *, InputEvent *);
	typedef void (__thiscall * ProcessButton_t)(void *, ButtonEvent *, void *);

	UInt32 * const kJumpHandler		= (UInt32 *)0x010D460C;
	UInt32 * const kMovementHandler	= (UInt32 *)0x010D4594;
	UInt32 * const kSneakHandler	= (UInt32 *)0x010D4644;
	enum { kSlot_CanProcess = 1, kSlot_ProcessButton = 4 };

	CanProcess_t	g_origJumpCan = NULL, g_origMoveCan = NULL, g_origSneakCan = NULL;
	ProcessButton_t	g_origJumpButton = NULL;

	// Se o ciclo de atualizacao parar (loading, menu), nao deixar os controlos presos.
	bool Busy()
	{
		if(g_state == kIdle) return false;
		if(GetTickCount() - g_lastTick > 2500)
		{
			_MESSAGE("gameplay: no skeleton updates for 2.5 s - releasing the controls and looking for the skeleton again");
			SceneHooksSetManualPose(false, kHang, 0, 1, 1);
			InterlockedExchange(&g_state, kIdle);
			SceneHooksRefreshTarget();
			return false;
		}
		return true;
	}

	bool __fastcall Hook_JumpCanProcess(void * handler, void *, InputEvent * event)	{ if(Busy()) return false; return g_origJumpCan(handler, event); }
	bool __fastcall Hook_MoveCanProcess(void * handler, void *, InputEvent * event)	{ if(Busy()) return false; return g_origMoveCan(handler, event); }
	bool __fastcall Hook_SneakCanProcess(void * handler, void *, InputEvent * event)	{ if(Busy()) return false; return g_origSneakCan(handler, event); }

	// O salto: junto a uma parede, agarra-se (ou passa por cima, se assim estiver configurado).
	bool TryStartFromJump()
	{
		Actor * player = ThePlayer();
		if(!player || SceneHooksIsPlaying()) return false;
		if(Engine::PlayerIsInMidair()) return false;

		void * ctrl = Engine::CharController(player);
		if(!ctrl || Engine::ControllerState(ctrl) != Engine::Controller::kState_OnGround) return false;

		if(!SceneHooksRefreshTarget()) return false;							// o jogo reconstroi o corpo (equipar, combate, celulas): reencontra-o

		float fx, fy;
		if(!SceneHooksRootForward(&fx, &fy)) return false;

		LoadConfig();															// afinacoes relidas: nao precisa de recompilar para afinar

		const NiPoint3 feet = Engine::Position(player);
		if(!g_cfg.alwaysCling)
		{
			const char * why = "";
			if(StartMantle(player, fx, fy, feet.x, feet.y, feet.z, &why)) return true;
		}
		return StartCling(player, ctrl, fx, fy);
	}

	void __fastcall Hook_JumpButton(void * handler, void *, ButtonEvent * event, void * data)
	{
		if(event)
		{
			if(g_state != kIdle) return;										// agarrado: o salto e tratado como "soltar"
			const float value = *(float *)&event->flags;
			const bool isDown = (value != 0.0f) && (event->timer == 0.0f);
			if(isDown && TryStartFromJump()) return;							// a pressao foi "consumida": nao ha salto
		}
		g_origJumpButton(handler, event, data);
	}

	class InputSink : public BSTEventSink<InputEvent>
	{
	public:
		virtual EventResult ReceiveEvent(InputEvent ** events, InputEventDispatcher * dispatcher)
		{
			InputStringHolder * strings = Engine::UserEvents();
			if(!events || !*events || !strings) return kEvent_Continue;

			for(InputEvent * e = *events; e; e = e->next)
			{
				if(e->eventType != InputEvent::kEventType_Button) continue;

				ButtonEvent * button = (ButtonEvent *)e;
				const BSFixedString * control = button->GetControlID();
				if(!control) continue;

				const float value = *(float *)&button->flags;
				const bool held = value != 0.0f;
				const bool pressedNow = held && button->timer == 0.0f;

				if(*control == strings->forward)			g_in.forward = held;
				else if(*control == strings->back)			g_in.back = held;
				else if(*control == strings->strafeLeft)	g_in.left = held;
				else if(*control == strings->strafeRight)	g_in.right = held;
				else if(*control == strings->jump)			{ if(pressedNow) g_in.jumpEdge = 1; }
				else if(*control == strings->sneak)			{ if(pressedNow) g_in.sneakEdge = 1; }
			}
			return kEvent_Continue;
		}
	};
	InputSink g_sink;
}

void InstallGameplay()
{
	DefaultConfig();
	memset((void *)&g_in, 0, sizeof(g_in));

	PatchSlot(kJumpHandler,		kSlot_CanProcess,	(void *)&Hook_JumpCanProcess,	&g_origJumpCan);
	PatchSlot(kMovementHandler,	kSlot_CanProcess,	(void *)&Hook_MoveCanProcess,	&g_origMoveCan);
	PatchSlot(kSneakHandler,	kSlot_CanProcess,	(void *)&Hook_SneakCanProcess,	&g_origSneakCan);
	PatchSlot(kJumpHandler,		kSlot_ProcessButton,(void *)&Hook_JumpButton,		&g_origJumpButton);

	SceneHooksSetTick(&OnTick);
	_MESSAGE("gameplay: installed - press JUMP facing a wall to cling to it");
}

void RegisterGameplayInput()
{
	InputEventDispatcher * dispatcher = *g_inputEventDispatcher;
	if(!dispatcher) { _MESSAGE("gameplay: input dispatcher not available - the climb keys will not work"); return; }
	dispatcher->AddEventSink(&g_sink);
	_MESSAGE("gameplay: input receiver registered");
}
