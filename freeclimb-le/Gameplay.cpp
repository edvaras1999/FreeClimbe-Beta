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
		float	attachReach;	// distancia maxima a parede para se agarrar (o personagem e puxado ate ela)
		int		hopEase;		// 1 = salto suave (comeca e acaba devagar); 0 = velocidade constante
		int		warp;			// 1 = o controlador e teletransportado a cada frame; 0 = tenta mover sem "warp" (camara mais suave?)
		int		entryMode;		// 0 = Salto sozinho junto a uma parede; 1 = Salto + W + A + D juntos (como o mod original)
		float	airReach;		// alcance para apanhar uma parede a meio de um salto
		float	wallRunSpeed, wallRunDiag;	// corrida na parede (Shift) e multiplicador na diagonal
		int		contextHops;	// 1 = salta sozinho para a parede seguinte quando a atual acaba a andar de lado
		float	catchSpeed;		// multiplica a velocidade da animacao de apanhar a parede
		int		poseMode;		// 1 = pose escrita nos locais ANTES da passagem do motor (como o original); 0 = modo antigo
		float	entryBlend, exitBlend, motionBlend;	// fusoes (segundos): entrar na pose, sair dela, trocar de movimento
		float	runHeight;		// sobe/desce o corpo durante a corrida na parede (unidades)
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
		g_cfg.hopScale = 1.8f;	g_cfg.hopSpeed = 1.1f;
		g_cfg.attachReach = 65.0f;	g_cfg.hopEase = 1;	g_cfg.warp = 1;
		g_cfg.entryMode = 0;	g_cfg.airReach = 90.0f;	g_cfg.wallRunSpeed = 379.5f;	g_cfg.wallRunDiag = 1.15f;
		g_cfg.contextHops = 1;	g_cfg.catchSpeed = 1.5f;
		g_cfg.poseMode = 1;	g_cfg.entryBlend = 0.14f;	g_cfg.exitBlend = 0.22f;	g_cfg.motionBlend = 0.15f;	g_cfg.runHeight = 0.0f;
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
			else if(KeyIs(key, "AttachReach"))	g_cfg.attachReach = f;
			else if(KeyIs(key, "HopEase"))		g_cfg.hopEase = atoi(value.c_str());
			else if(KeyIs(key, "Warp"))			g_cfg.warp = atoi(value.c_str());
			else if(KeyIs(key, "EntryMode"))	g_cfg.entryMode = atoi(value.c_str());
			else if(KeyIs(key, "AirReach"))		g_cfg.airReach = f;
			else if(KeyIs(key, "WallRunSpeed"))	g_cfg.wallRunSpeed = f;
			else if(KeyIs(key, "WallRunDiagonal"))	g_cfg.wallRunDiag = f;
			else if(KeyIs(key, "ContextHops"))	g_cfg.contextHops = atoi(value.c_str());
			else if(KeyIs(key, "CatchSpeed"))	g_cfg.catchSpeed = f;
			else if(KeyIs(key, "PoseMode"))	g_cfg.poseMode = atoi(value.c_str());
			else if(KeyIs(key, "EntryBlend"))	g_cfg.entryBlend = f;
			else if(KeyIs(key, "ExitBlend"))	g_cfg.exitBlend = f;
			else if(KeyIs(key, "MotionBlend"))	g_cfg.motionBlend = f;
			else if(KeyIs(key, "RunHeight"))	g_cfg.runHeight = f;
		}

		SceneHooksConfigure(g_cfg.poseMode, g_cfg.entryBlend, g_cfg.exitBlend);
		_MESSAGE("gameplay: pose engine: mode %d (%s), blend in/out/motion %.2f/%.2f/%.2f s, run height %.0f", g_cfg.poseMode, g_cfg.poseMode ? "locals before the engine pass" : "OLD: world after the pass", (double)g_cfg.entryBlend, (double)g_cfg.exitBlend, (double)g_cfg.motionBlend, (double)g_cfg.runHeight);
		_MESSAGE("gameplay: settings: reach %.0f, wall gap %.0f, speeds up/down/side %.0f/%.0f/%.0f, ledge %.0f-%.0f, head %.0f, alwaysCling %d, faceWall %d, hop scale/speed %.2f/%.2f, attach reach %.0f, ease %d, warp %d, ENTRY MODE %d (%s)",
				 (double)g_cfg.reach, (double)g_cfg.gap, (double)g_cfg.climbSpeed, (double)g_cfg.downSpeed, (double)g_cfg.sideSpeed,
				 (double)g_cfg.minLedge, (double)g_cfg.maxLedge, (double)g_cfg.headHeight, g_cfg.alwaysCling, g_cfg.faceWall, (double)g_cfg.hopScale, (double)g_cfg.hopSpeed, (double)g_cfg.attachReach, g_cfg.hopEase, g_cfg.warp, g_cfg.entryMode, g_cfg.entryMode == 0 ? "Jump alone" : "Jump + W + A + D together");
	}

	// ------------------------------------------------------------------ estado
	enum State { kIdle = 0, kCling = 1, kTopOut = 2, kHop = 3, kCatch = 4 };

	// os movimentos do pacote do FreeClimb
	const int	kHang = 1, kUp = 2, kDown = 3, kLeft = 4, kRight = 5, kMantle = 42;
	const float	kUpNatural = 90.3f / 2.0f;			// velocidade natural da animacao "up"/"down" (unidades/s)
	const float	kSideNatural = 42.3f / 1.17f;		// idem "left"/"right"
	const float	kMantleForward = 62.9f, kMantleUp = 132.3f;
	const int	kHopLeft = 9, kHopRight = 10, kHopUp = 11;
	const int	kReach = 8, kDrop = 15, kJumpCatch = 16, kSprintCatch = 17, kLedgeCatch = 19;
	const int	kRunUp = 20, kRunLeft = 21, kRunRight = 22, kRunDiagLeft = 23, kRunDiagRight = 24, kRunLaunch = 25, kRunCatch = 26;
	const int	kRunLaunchLeft = 34, kRunLaunchRight = 35, kSideBrace = 36, kBackFlipOut = 37, kCtxHopLeft = 40, kCtxHopRight = 41;
	const float	kRunNatural = 267.9f / 0.63f;				// velocidade natural dos ciclos de corrida
	const float	kCtxHopDist = 104.0f;
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
	float			g_hopCom = 1.0f;
	DWORD			g_catchStart = 0, g_bridgeStart = 0, g_dropStart = 0;
	int				g_catchMotion = 0, g_bridgeMotion = 0, g_dropMotion = 0;
	float			g_catchSecs = 1.0f, g_bridgeSecs = 0.0f, g_dropSecs = 0.0f, g_catchFrom[3], g_catchTo[3];
	bool			g_wasRun = false;
	float			g_hopSecs = 1.0f, g_hopFrom[3], g_hopTo[3];
	float			g_mantleStartPos[3], g_mantleDir[2], g_mantleScaleY = 1.0f, g_mantleScaleZ = 1.0f, g_mantleSecs = 1.87f;
	float			g_rest[99 * 10], g_buf[99 * 10];
	bool			g_restOk = false;
	float			g_pose[99 * 10];
	float			g_runBlend = 0.0f;			// 0 = a trepar, 1 = a correr (mistura das velocidades)
	bool			g_runReset = true;			// primeiro quadro de uma corrida: a orientacao do corpo parte da desejada
	int				g_lastRunMotion = 20;		// ultima direcao de corrida (para o runCatch)
	float			g_inX = 0.0f, g_inY = 0.0f;	// entrada de movimento (x: direita+, y: cima+), normalizada
	int				g_curStyle = 0, g_curDir = 0;	// como mostrar o movimento atual da parede
	float			g_curDt = 0.016f;
	unsigned char	g_buildWarned[64];
	int				g_bridgeDirMotion = 0, g_showMotion = 1;
	float			g_showPhase = 0.0f, g_showBridge = 0.0f;

	// Um movimento do pacote que pode nao existir (ex.: sprintCatch foi retirado do pacote 0.5): usa o substituto.
	int Avail(int motion, int fallback)
	{
		return FC_MotionSeconds(motion) > 0.0f ? motion : fallback;
	}

	// Constroi a pose final (amostra + colocacao na parede, como o original) e entrega-a ao motor de pose.
	void Show(int motion, float phase, int direction, int style, float dirX = 0.0f, float dirY = 0.0f, float dt = 0.016f, bool resetRun = false, float blend = -1.0f)
	{
		int m = motion;
		if(!FC_BuildPose(m, phase, direction, style, g_cfg.gap, dirX, dirY, dt, resetRun, g_pose))
		{
			if(m >= 0 && m < 64 && !g_buildWarned[m]) { g_buildWarned[m] = 1; _MESSAGE("gameplay: WARNING - could not build a pose for motion %d (style %d, direction %d); showing the hanging pose instead", m, style, direction); }
			m = kHang; direction = 0; style = 0;
			if(!FC_BuildPose(m, 0.0f, 0, 0, g_cfg.gap, 0, 0, dt, false, g_pose)) return;
		}
		if(style == FC_STYLE_RUN) g_pose[2] += g_cfg.runHeight;		// afinacao opcional da altura do corpo na corrida
		SceneHooksSetPose(true, g_pose, m * 16 + (direction & 15), blend < 0.0f ? g_cfg.motionBlend : blend);
	}

	// Mostra o movimento da parede escolhido neste quadro (ciclo, ou sequencia de ponte da corrida).
	void ShowCling()
	{
		if(g_showBridge > 0.5f)
		{
			const int style = (g_showMotion == kRunLaunch || g_showMotion == kRunLaunchLeft || g_showMotion == kRunLaunchRight) ? FC_STYLE_WALLFEET : FC_STYLE_WALL;
			Show(g_showMotion, g_showPhase, g_bridgeDirMotion, style, 0.0f, 0.0f, g_curDt, false);
			return;
		}
		Show(g_showMotion, g_showPhase, g_curDir, g_curStyle, g_inX, g_inY, g_curDt, g_curStyle == FC_STYLE_RUN && g_runReset);
		if(g_curStyle == FC_STYLE_RUN) g_runReset = false;
	}

	void HidePose()
	{
		SceneHooksSetPose(false, NULL, 0, 0.0f);
	}

	struct InputState { volatile LONG forward, back, left, right, sprint, jumpEdge, sneakEdge; };
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
		Engine::ControllerSetPosition(ctrl, target, true, g_cfg.warp != 0);

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

	// Ate onde se pode ir a partir de (px,py,pz+h) na direcao (dx,dy,dz) (unitaria), em 'maxDist': devolve a distancia
	// livre (maxDist se nada a bloqueia). Testa a varias alturas e fica com a mais curta.
	float FreeDistance(Actor * player, float px, float py, float pz, float dx, float dy, float dz, float maxDist, const float * heights, int count)
	{
		float free = maxDist;
		for(int k = 0; k < count; ++k)
		{
			const Havok::Result r = Havok::RayCast(NiPoint3(px, py, pz + heights[k]), NiPoint3(dx, dy, dz), maxDist, Havok::kLayer_Transparent, player);
			if(r.didHit && r.distance < free) free = r.distance;
		}
		return free;
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
		HidePose();
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
		Show(kMantle, phase, 0, FC_STYLE_BODY);			// so as rotacoes: o personagem leva o corpo
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

		Show(kMantle, 0.0f, 0, FC_STYLE_BODY);
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
		HidePose();
		InterlockedExchange(&g_state, kIdle);
		_MESSAGE("gameplay: released from the wall (%s)", why);
	}

	// O estado "agarrado", a partir da posicao (g_pos) e da normal (g_n) atuais.
	void EnterCling()
	{
		g_phase = 0.0f; g_motion = kHang; g_wasRun = false; g_bridgeSecs = 0.0f; g_runBlend = 0.0f; g_runReset = true;
		g_attachTime = g_lastTick = GetTickCount();
		g_in.jumpEdge = 0; g_in.sneakEdge = 0;
		g_curStyle = FC_STYLE_WALL; g_curDir = 0;
		Show(kHang, 0.0f, 0, FC_STYLE_WALL);
		InterlockedExchange(&g_state, kCling);
	}

	// Chegou a parede (fim de um salto ou de uma animacao de apanhar): se ha parede aqui, agarra-se; senao, larga.
	void LandOnWall(Actor * player, void * ctrl, bool afterUpHop)
	{
		static const float chest[1] = { 70.0f };
		const WallHit w = ProbeWall(player, -g_n[0], -g_n[1], g_pos[0], g_pos[1], g_pos[2], g_cfg.gap + 50.0f, chest, 1);
		if(!w.found)
		{
			// aterrou acima do topo da parede: se foi um salto para cima, passa por cima da borda em vez de cair
			const char * why = "";
			if(afterUpHop && StartMantle(player, -g_n[0], -g_n[1], g_pos[0], g_pos[1], g_pos[2], &why)) return;
			Release("missed the wall");
			return;
		}

		g_n[0] = w.nx; g_n[1] = w.ny;
		g_pos[0] = w.x + g_n[0] * g_cfg.gap;
		g_pos[1] = w.y + g_n[1] * g_cfg.gap;
		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
		EnterCling();
	}

	// Entrada na parede: o personagem e levado ate ela enquanto toca a animacao de apanhar (reach, jumpCatch, ...).
	void BeginCatch(int motion, const WallHit & wall, float fromX, float fromY, float fromZ)
	{
		const float secs = FC_MotionSeconds(motion);
		g_n[0] = wall.nx; g_n[1] = wall.ny;
		g_catchFrom[0] = fromX; g_catchFrom[1] = fromY; g_catchFrom[2] = fromZ;
		g_catchTo[0] = wall.x + g_n[0] * g_cfg.gap;
		g_catchTo[1] = wall.y + g_n[1] * g_cfg.gap;
		g_catchTo[2] = fromZ;
		g_catchMotion = motion;
		g_catchSecs = (secs > 0.0f ? secs : 0.1f) / (g_cfg.catchSpeed > 0.2f ? g_cfg.catchSpeed : 1.0f);
		g_catchStart = g_lastTick = GetTickCount();
		g_pos[0] = fromX; g_pos[1] = fromY; g_pos[2] = fromZ;

		g_in.jumpEdge = 0; g_in.sneakEdge = 0;
		Show(motion, 0.0f, 0, FC_STYLE_BODY);				// so rotacoes: o personagem move-se mesmo
		InterlockedExchange(&g_state, kCatch);
	}

	void TickCatch(Actor * player, void * ctrl, DWORD now)
	{
		float p = (float)(now - g_catchStart) / 1000.0f / g_catchSecs;
		if(p > 1.0f) p = 1.0f;
		const float e = p * p * (3.0f - 2.0f * p);
		for(int k = 0; k < 3; ++k) g_pos[k] = g_catchFrom[k] + (g_catchTo[k] - g_catchFrom[k]) * e;

		if(g_cfg.faceWall) Engine::Rotation(player).z = atan2f(-g_n[0], -g_n[1]);
		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
		Show(g_catchMotion, p, 0, FC_STYLE_BODY);
		if(p >= 1.0f) LandOnWall(player, ctrl, false);
	}

	// Procura a parede e, se a encontra, comeca a apanha-la. 'air' = a meio de um salto.
	bool StartEntry(Actor * player, float fx, float fy, bool air)
	{
		const NiPoint3 feet = Engine::Position(player);
		static const float heights[3] = { 55.0f, 95.0f, 135.0f };
		const WallHit wall = ProbeWall(player, fx, fy, feet.x, feet.y, feet.z, air ? g_cfg.airReach : g_cfg.attachReach, heights, 3);
		if(!wall.found) return false;

		if(!air)
		{
			// um obstaculo REAL (parede, pilar) a altura dos joelhos e claramente antes da parede: nao agarra.
			// (Declives e entulho baixo nao contam.)
			const Havok::Result low = Havok::RayCast(NiPoint3(feet.x, feet.y, feet.z + 25.0f), NiPoint3(fx, fy, 0.0f), wall.dist, Havok::kLayer_Transparent, player);
			if(low.didHit && fabsf(low.normal.z) < 0.5f && low.distance < wall.dist - 15.0f)
			{
				_MESSAGE("gameplay: not clinging - a wall-like obstacle is in the way at knee height");
				return false;
			}
		}

		int motion;
		if(air)
		{
			motion = g_in.sprint ? Avail(kSprintCatch, kJumpCatch) : kJumpCatch;
			Ledge ledge; const char * why = "";
			if(!g_in.sprint && FindLedge(player, fx, fy, feet.x, feet.y, feet.z, &ledge, &why)) motion = kLedgeCatch;
		}
		else motion = (wall.dist <= 48.0f) ? kReach : kJumpCatch;

		BeginCatch(motion, wall, feet.x, feet.y, feet.z);
		_MESSAGE("gameplay: CATCHING the wall (%s, %.0f units ahead) with animation %d. W/S climb, A/D sidestep, Shift = wall run.",
				 air ? "in the air" : "from the ground", (double)wall.dist, motion);
		return true;
	}

	// Largar com uma animacao: o personagem cai (ou e empurrado) enquanto o corpo toca drop/backFlipOut.
	void StartDrop(int motion, float pushSpeed, const char * why)
	{
		Release(why, pushSpeed);
		const float secs = FC_MotionSeconds(motion);
		if(secs <= 0.0f) return;
		g_dropMotion = motion; g_dropStart = GetTickCount(); g_dropSecs = secs;
		Show(motion, 0.0f, 0, motion == kBackFlipOut ? FC_STYLE_BACKFLIP : FC_STYLE_BODY);
	}

	// ------------------------------------------------------------------ saltos na parede (movimento + salto)
	// 'context' = salto automatico entre paredes: o centro do corpo nao e deslocado pela animacao (o personagem move-se mesmo).
	void StartHop(int motion, float lateral, float vertical, bool context = false)
	{
		const float secs = FC_MotionSeconds(motion);
		if(secs <= 0.0f) { Release("hop animation not available"); return; }

		const float rx = -g_n[1], ry = g_n[0];									// a direita ao longo da parede
		float dxs = rx * lateral * g_cfg.hopScale, dys = ry * lateral * g_cfg.hopScale, dzs = vertical * g_cfg.hopScale;

		// o caminho do salto tem de estar livre: se algo o bloqueia, o salto encurta (ou nao acontece)
		const float total = sqrtf(dxs * dxs + dys * dys + dzs * dzs);
		if(total > 1.0f)
		{
			static const float body[3] = { 30.0f, 75.0f, 120.0f };
			const float free = FreeDistance(ThePlayer(), g_pos[0], g_pos[1], g_pos[2], dxs / total, dys / total, dzs / total, total + 25.0f, body, 3);
			if(free < total + 25.0f)
			{
				const float allowed = free - 25.0f;
				if(allowed < 25.0f) { _MESSAGE("gameplay: hop blocked by an obstacle - staying on the wall"); return; }
				const float k = allowed / total;
				dxs *= k; dys *= k; dzs *= k;
				_MESSAGE("gameplay: hop shortened by an obstacle (%.0f%%)", (double)(k * 100.0f));
			}
		}

		g_hopFrom[0] = g_pos[0]; g_hopFrom[1] = g_pos[1]; g_hopFrom[2] = g_pos[2];
		g_hopTo[0] = g_pos[0] + dxs;
		g_hopTo[1] = g_pos[1] + dys;
		g_hopTo[2] = g_pos[2] + dzs;
		g_hopMotion = motion;
		g_hopCom = context ? 0.0f : 1.0f;
		g_hopSecs = secs / (g_cfg.hopSpeed > 0.1f ? g_cfg.hopSpeed : 1.0f);
		g_hopStart = g_lastTick = GetTickCount();

		Show(motion, 0.0f, 0, context ? FC_STYLE_BODY : FC_STYLE_WALL);
		InterlockedExchange(&g_state, kHop);
		_MESSAGE("gameplay: HOP (%s%s) %.0f units", context ? "automatic, " : "", motion == kHopUp ? "up" : (lateral < 0.0f ? "left" : "right"),
				 (double)(fabsf(lateral) + fabsf(vertical)) * g_cfg.hopScale);
	}

	void TickHop(Actor * player, void * ctrl, DWORD now)
	{
		float p = (float)(now - g_hopStart) / 1000.0f / g_hopSecs;
		if(p > 1.0f) p = 1.0f;
		const float e = g_cfg.hopEase ? p * p * (3.0f - 2.0f * p) : p;			// suave no inicio e no fim (ou constante)

		for(int k = 0; k < 3; ++k) g_pos[k] = g_hopFrom[k] + (g_hopTo[k] - g_hopFrom[k]) * e;
		if(g_cfg.faceWall) Engine::Rotation(player).z = atan2f(-g_n[0], -g_n[1]);
		PlaceController(ctrl, g_pos[0], g_pos[1], g_pos[2]);
		Show(g_hopMotion, p, 0, g_hopCom > 0.5f ? FC_STYLE_WALL : FC_STYLE_BODY);
		if(p >= 1.0f) { _MESSAGE("gameplay: hop landing"); LandOnWall(player, ctrl, g_hopMotion == kHopUp); }
	}

	// A cada atualizacao do esqueleto do jogador (thread de trabalho do jogo).
	void OnTick()
	{
		if(g_state == kIdle)
		{
			// a animacao de largar (drop / backFlipOut) acaba sozinha
			if(g_dropMotion)
			{
				const float ph = (float)(GetTickCount() - g_dropStart) / 1000.0f / g_dropSecs;
				if(ph >= 1.0f) { HidePose(); g_dropMotion = 0; }
				else Show(g_dropMotion, ph, 0, g_dropMotion == kBackFlipOut ? FC_STYLE_BACKFLIP : FC_STYLE_BODY);
			}

			// apanhar uma parede a meio de um salto: salto premido no ar (com a combinacao de entrada, se configurada)
			if(InterlockedExchange(&g_in.jumpEdge, 0) && !g_dropMotion)
			{
				Actor * player = ThePlayer();
				float fx, fy;
				if(player && Engine::PlayerIsInMidair() && SceneHooksRootForward(&fx, &fy))
				{
					LoadConfig();
					const bool combo = (g_cfg.entryMode == 0) || (g_in.forward && g_in.left && g_in.right);
					if(combo) StartEntry(player, fx, fy, true);
				}
			}
			return;
		}

		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(!ctrl) { Release("no controller"); return; }

		if(SceneHooksPoseFaulted()) { Release("the skeleton was rebuilt by the game"); return; }

		const DWORD now = GetTickCount();
		if(g_state == kTopOut) { g_lastTick = now; TickMantle(ctrl, now); return; }
		if(g_state == kHop) { g_lastTick = now; TickHop(player, ctrl, now); return; }
		if(g_state == kCatch) { g_lastTick = now; TickCatch(player, ctrl, now); return; }

		float dt = (float)(now - g_lastTick) / 1000.0f;
		if(dt > 0.1f) dt = 0.1f;
		g_lastTick = now;

		// soltar: salto ou agachar (ignora a pressao que acabou de nos agarrar)
		const LONG jump = InterlockedExchange(&g_in.jumpEdge, 0), sneak = InterlockedExchange(&g_in.sneakEdge, 0);
		const bool up = g_in.forward && !g_in.back, down = g_in.back && !g_in.forward;
		const bool left = g_in.left && !g_in.right, right = g_in.right && !g_in.left;

		if(now - g_attachTime > 400 && (jump || sneak))
		{
			if(sneak && !jump)	{ StartDrop(kDrop, 0.0f, "sneak"); return; }
			// Como no mod original: A+S+D + Salto larga no sitio; direcao + Salto salta; S + Salto empurra-se para longe.
			if(g_in.back && g_in.left && g_in.right)	{ StartDrop(kDrop, 0.0f, "let go in place"); return; }
			if(up)			{ StartHop(kHopUp, 0.0f, kHopUpDist); return; }
			if(left)		{ StartHop(kHopLeft, kHopLeftDist, 0.0f); return; }
			if(right)		{ StartHop(kHopRight, kHopRightDist, 0.0f); return; }
			if(down)		{ StartDrop(kBackFlipOut, 170.0f, "pushed away from the wall"); return; }
			if(g_cfg.entryMode == 0) { StartDrop(kDrop, 0.0f, "let go in place"); return; }		// modo simples: Salto sozinho larga
			return;																				// modo original: Salto sozinho nao faz nada
		}

		// ---- entrada de movimento: um vetor (x: direita+, y: cima+), como no original; diagonais (W+A, W+D) contam
		float ix = (g_in.right ? 1.0f : 0.0f) - (g_in.left ? 1.0f : 0.0f);
		float iy = (g_in.forward ? 1.0f : 0.0f) - (g_in.back ? 1.0f : 0.0f);
		{ const float len = sqrtf(ix * ix + iy * iy); if(len > 1.0f) { ix /= len; iy /= len; } }
		g_inX = ix; g_inY = iy;
		const bool moving = fabsf(ix) + fabsf(iy) > 0.1f;
		const bool wantRun = g_in.sprint && iy >= 0.0f && moving;

		// mistura trepar -> correr (suave, para a velocidade nao saltar)
		{
			const float target = wantRun ? 1.0f : 0.0f;
			float step = target - g_runBlend;
			const float up2 = dt / 0.36f, down2 = dt / 0.32f;
			if(step > up2) step = up2;
			if(step < -down2) step = -down2;
			g_runBlend += step;
			if(iy < 0.0f) g_runBlend = 0.0f;
		}
		const float mode = g_runBlend * g_runBlend * (3.0f - 2.0f * g_runBlend);
		const bool diagonal = fabsf(ix) > 0.1f && fabsf(iy) > 0.1f;
		const float runSp = g_cfg.wallRunSpeed * (diagonal ? g_cfg.wallRunDiag : 1.0f);
		const float vertical = iy > 0.0f ? g_cfg.climbSpeed : g_cfg.downSpeed;

		float vt = ix * (g_cfg.sideSpeed + (runSp - g_cfg.sideSpeed) * mode);
		float vz = iy * (vertical + (runSp - vertical) * mode);

		// que movimento do pacote mostrar (como o original: o eixo dominante escolhe o ciclo)
		int motion = kHang, runDir = 0, style = FC_STYLE_WALL;
		if(wantRun)
		{
			style = FC_STYLE_RUN;
			if(fabsf(ix) < 0.1f)			motion = kRunUp;
			else if(fabsf(iy) < 0.1f)	motion = ix < 0.0f ? kRunLeft : kRunRight;
			else							motion = ix < 0.0f ? kRunDiagLeft : kRunDiagRight;
			runDir = motion;
		}
		else if(moving)
		{
			if(fabsf(iy) > fabsf(ix))	motion = iy < 0.0f ? kDown : kUp;
			else						motion = ix < 0.0f ? kLeft : kRight;
		}

		// ao comecar ou acabar de correr (ou ao mudar de lado), toca-se a sequencia de ponte do pacote
		const bool lastLeft = g_lastRunMotion == kRunLeft || g_lastRunMotion == kRunDiagLeft, lastRight = g_lastRunMotion == kRunRight || g_lastRunMotion == kRunDiagRight;
		const bool nowLeft = motion == kRunLeft || motion == kRunDiagLeft, nowRight = motion == kRunRight || motion == kRunDiagRight;
		const bool sideFlip = wantRun && g_wasRun && ((lastLeft && nowRight) || (lastRight && nowLeft));
		int bridgeDir = 0;
		if(wantRun != g_wasRun || sideFlip)
		{
			if(wantRun && !g_wasRun)
			{
				g_bridgeMotion = fabsf(ix) > 0.1f ? (ix < 0.0f ? kRunLaunchLeft : kRunLaunchRight) : kRunLaunch;
				bridgeDir = motion; g_runReset = true;
			}
			else if(!wantRun && g_wasRun) { g_bridgeMotion = kRunCatch; bridgeDir = g_lastRunMotion; }
			else { g_bridgeMotion = kSideBrace; bridgeDir = motion; }
			g_wasRun = wantRun;
			g_bridgeSecs = FC_MotionSecondsDir(g_bridgeMotion, bridgeDir);
			g_bridgeDirMotion = bridgeDir;
			g_bridgeStart = now;
		}
		if(wantRun) g_lastRunMotion = motion;

		if(motion != g_motion) { g_motion = motion; g_phase = 0.0f; }

		// o ciclo avanca ao ritmo do movimento real (velocidade / distancia por ciclo do clip)
		float rate = 0.0f;
		if(motion != kHang)
		{
			float stride = FC_MotionStride(motion);
			if(stride < 1.0f) stride = (motion == kUp || motion == kDown) ? 90.3f : (motion == kLeft || motion == kRight) ? 42.3f : 267.9f;
			const float speed = sqrtf(vt * vt + vz * vz);
			g_phase += dt * speed / stride;
			if(!(g_phase == g_phase)) g_phase = 0.0f;
			while(g_phase >= 1.0f) g_phase -= 1.0f;
			rate = speed;
		}
		else g_phase = 0.0f;

		// pose a mostrar neste quadro
		g_curDt = dt;
		g_curStyle = style; g_curDir = runDir;
		g_showMotion = motion; g_showPhase = g_phase; g_showBridge = 0.0f;
		if(g_bridgeSecs > 0.0f && (float)(now - g_bridgeStart) / 1000.0f < g_bridgeSecs)
		{
			g_showMotion = g_bridgeMotion;
			g_showPhase = (float)(now - g_bridgeStart) / 1000.0f / g_bridgeSecs;
			g_showBridge = 1.0f;
		}

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

		// nao atravessar tetos nem obstaculos: o movimento so avanca ate onde esta livre
		if(vz > 0.0f)
		{
			static const float headTop[1] = { 0.0f };
			const float room = FreeDistance(player, g_pos[0], g_pos[1], g_pos[2] + g_cfg.headHeight, 0.0f, 0.0f, 1.0f, vz * dt + 12.0f, headTop, 1);
			if(room < vz * dt + 12.0f) vz = 0.0f;										// ha um teto/borda por cima
		}
		if(vt != 0.0f)
		{
			static const float sideH[3] = { 30.0f, 70.0f, 120.0f };
			const float tx = (vt > 0.0f) ? -g_n[1] : g_n[1], ty = (vt > 0.0f) ? g_n[0] : -g_n[0];
			const float room = FreeDistance(player, g_pos[0], g_pos[1], g_pos[2], tx, ty, 0.0f, fabsf(vt) * dt + 20.0f, sideH, 3);
			if(room < fabsf(vt) * dt + 20.0f) vt = 0.0f;								// obstaculo ao lado (a parede em L e tratada acima)
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
					ShowCling();
					_MESSAGE("gameplay: outer corner - wrapped around to the next face");
					return;
				}
			}
			if(vt != 0.0f && g_cfg.contextHops)
			{
				// ha outra parede ao alcance de um salto, do lado para onde andava? Salta sozinho para ela.
				const float sign = vt > 0.0f ? 1.0f : -1.0f;
				const float dx = g_pos[0] + (-g_n[1]) * sign * kCtxHopDist * g_cfg.hopScale;
				const float dy = g_pos[1] + ( g_n[0]) * sign * kCtxHopDist * g_cfg.hopScale;
				static const float chestC[1] = { 70.0f };
				const WallHit dest = ProbeWall(player, -g_n[0], -g_n[1], dx, dy, g_pos[2], g_cfg.gap + 60.0f, chestC, 1);
				if(dest.found) { StartHop(sign > 0.0f ? kCtxHopRight : kCtxHopLeft, sign * kCtxHopDist, 0.0f, true); return; }
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
				g_motion = kHang; g_phase = 0.0f; rate = 0.0f; g_showMotion = kHang; g_showPhase = 0.0f; g_showBridge = 0.0f; g_curStyle = FC_STYLE_WALL; g_curDir = 0;
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
		ShowCling();
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
			HidePose();
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

		// modo original: so entra com Salto + W + A + D premidos juntos (evita entrar sem querer)
		if(g_cfg.entryMode != 0 && !(g_in.forward && g_in.left && g_in.right))
		{
			static DWORD lastWarn = 0;
			if(GetTickCount() - lastWarn > 4000)
			{
				lastWarn = GetTickCount();
				_MESSAGE("gameplay: jump ignored - EntryMode=%d needs Jump + W + A + D held together (keys seen: W %d, A %d, D %d). Set EntryMode=0 in FreeClimbLE.ini to enter with Jump alone.",
						 g_cfg.entryMode, (int)g_in.forward, (int)g_in.left, (int)g_in.right);
			}
			return false;
		}

		const NiPoint3 feet = Engine::Position(player);
		if(!g_cfg.alwaysCling)
		{
			const char * why = "";
			if(StartMantle(player, fx, fy, feet.x, feet.y, feet.z, &why)) return true;
		}
		return StartEntry(player, fx, fy, false);
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
				else if(*control == strings->sprint)		g_in.sprint = held;
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

	LoadConfig();
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
