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
#include "Options.h"

#include "skse/GameInput.h"
#include "skse/GameReferences.h"

namespace
{
	// ------------------------------------------------------------------ afinacoes (FreeClimbLE.ini)
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

	void FollowersReleaseAll();							// definida mais abaixo
	void FollowersPlaceAtPlayer();
	volatile LONG	g_state = kIdle;
	DWORD			g_lastTick = 0, g_attachTime = 0;
	float			g_pos[3];			// pes do personagem enquanto agarrado (posicao LOGICA)

	// ---- passo A: suavizacao visual ----
	// A logica (g_pos, g_n) continua a ser exata; o que se MOSTRA segue-a com um desvio que desvanece sem ressalto.
	// Assim nao ha atraso a andar em linha reta, e os saltos (cantos, facetas de arvores/pilares) deixam de ser teleportes.
	float			g_off[2] = { 0.0f, 0.0f }, g_offVel[2] = { 0.0f, 0.0f };
	float			g_yawOff = 0.0f, g_yawOffVel = 0.0f, g_prevGoalYaw = 0.0f;
	bool			g_yawInit = false, g_prevGoalValid = false;
	float			g_moveT = 0.0f;
	float			g_fitDelta[4] = { 0, 0, 0, 0 };		// quanto cada membro deve avancar para tocar na superficie medida
	int				g_fitRR = 0;
	float			g_fpsAvg = 60.0f;
	float			g_prevContact[4] = { 0, 0, 0, 0 };
	int				g_prevContactMotion = 0;
	LONGLONG		g_qpcLast = 0, g_qpcFaceLast = 0, g_qpcViewLast = 0;

	float QpcSeconds(LONGLONG * last)
	{
		LARGE_INTEGER f, n;
		QueryPerformanceFrequency(&f); QueryPerformanceCounter(&n);
		float dt = (*last != 0 && f.QuadPart > 0) ? (float)((double)(n.QuadPart - *last) / (double)f.QuadPart) : 0.016f;
		*last = n.QuadPart;
		if(!(dt > 0.0f)) dt = 0.001f;
		if(dt > 0.1f) dt = 0.1f;
		return dt;
	}

	float WrapPi(float a)
	{
		while(a > 3.14159265f) a -= 6.2831853f;
		while(a < -3.14159265f) a += 6.2831853f;
		return a;
	}

	// Faz 'cur' tender para 0 como uma mola criticamente amortecida (sem ressalto), com velocidade maxima.
	float DecayToZero(float cur, float * vel, float smoothTime, float maxSpeed, float dt)
	{
		if(smoothTime < 0.0001f) { *vel = 0.0f; return 0.0f; }
		const float omega = 2.0f / smoothTime, x = omega * dt;
		const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
		float change = cur;
		const float maxChange = maxSpeed * smoothTime;
		if(change > maxChange) change = maxChange;
		if(change < -maxChange) change = -maxChange;
		const float target = cur - change;
		const float temp = (*vel + omega * change) * dt;
		*vel = (*vel - omega * temp) * e;
		float out = target + (change + temp) * e;
		if((cur > 0.0f && out < 0.0f) || (cur < 0.0f && out > 0.0f)) { out = 0.0f; *vel = 0.0f; }
		return out;
	}

	void ResetSmooth()
	{
		g_off[0] = g_off[1] = g_offVel[0] = g_offVel[1] = 0.0f;
		g_yawOff = g_yawOffVel = 0.0f;
		g_yawInit = false; g_prevGoalValid = false;
		g_qpcFaceLast = g_qpcViewLast = 0;
	}

	// Poe a posicao logica num novo sitio. Se foi um salto (canto, faceta), o que se mostra continua onde estava e vai ate la suavemente.
	void SnapPos(float x, float y)
	{
		const float dx = g_pos[0] - x, dy = g_pos[1] - y;
		if(g_cfg.posSmooth > 0.001f && dx * dx + dy * dy > 1.5f * 1.5f)
		{
			g_off[0] += dx; g_off[1] += dy;
			const float len = sqrtf(g_off[0] * g_off[0] + g_off[1] * g_off[1]);
			if(len > 90.0f) { g_off[0] *= 90.0f / len; g_off[1] *= 90.0f / len; }
		}
		g_pos[0] = x; g_pos[1] = y;
	}

	// Antes de uma transicao que usa g_pos como ponto de partida: o que se ve passa a ser a posicao logica.
	void CommitView()
	{
		g_pos[0] += g_off[0]; g_pos[1] += g_off[1];
		g_off[0] = g_off[1] = g_offVel[0] = g_offVel[1] = 0.0f;
	}

	// ---- estamina (propria do FreeClimb; valores no .ini/menu, em % da estamina maxima)
	float			g_staminaPct = 100.0f;
	bool			g_exhausted = false;
	DWORD			g_flashTick = 0;

	void FlashStaminaBar()
	{
		const DWORD now = GetTickCount();
		if(now - g_flashTick < 1200) return;
		g_flashTick = now;
		Engine::FlashMeter(Engine::kActorValue_Stamina);
	}

	// Atualiza a percentagem e o estado de exaustao; devolve a estamina atual e o maximo.
	bool StaminaRead(Actor * player, float * cur, float * mx)
	{
		*mx = Engine::GetMaxActorValue(player, Engine::kActorValue_Stamina);
		*cur = Engine::GetActorValue(player, Engine::kActorValue_Stamina);
		if(!(*mx > 1.0f)) return false;
		g_staminaPct = 100.0f * *cur / *mx;
		return true;
	}

	// Gasta 'ratePct' (% do maximo por segundo) durante 'dt'. Devolve true se esta exausto (escorrega em vez de subir).
	bool StaminaTick(Actor * player, float dt, float ratePct)
	{
		if(!g_cfg.staminaOn) { g_exhausted = false; return false; }
		float cur, mx;
		if(!StaminaRead(player, &cur, &mx)) return false;

		if(g_exhausted && g_staminaPct >= g_cfg.stResume) { g_exhausted = false; _MESSAGE("gameplay: stamina recovered - can climb again"); }
		if(!g_exhausted && cur <= 0.5f)
		{
			g_exhausted = true;
			FlashStaminaBar();
			_MESSAGE("gameplay: out of stamina - sliding down");
		}
		if(g_exhausted) return true;

		float damage = mx * ratePct * 0.01f * g_cfg.stScale * dt;
		if(damage > cur) damage = cur;
		if(damage > 0.0f) Engine::DamageActorValue(player, Engine::kActorValue_Stamina, damage);
		return false;
	}

	// Custo fixo de uma acao (salto, passar a borda). Sem 'force', recusa se estiver exausto ou nao chegar.
	bool StaminaSpend(Actor * player, float pct, bool force)
	{
		if(!g_cfg.staminaOn || pct <= 0.0f) return true;
		float cur, mx;
		if(!StaminaRead(player, &cur, &mx)) return true;

		float cost = mx * pct * 0.01f * g_cfg.stScale;
		if(!force && (g_exhausted || cur < cost * 0.5f)) { FlashStaminaBar(); return false; }
		if(cost > cur) cost = cur;
		if(cost > 0.0f) Engine::DamageActorValue(player, Engine::kActorValue_Stamina, cost);
		return true;
	}

	// Velocidade natural de um clip (unidades/s): distancia por ciclo / duracao.
	float NaturalSpeed(int motion, float fallback)
	{
		const float stride = FC_MotionStride(motion), secs = FC_MotionSeconds(motion);
		return (stride > 1.0f && secs > 0.05f) ? stride / secs : fallback;
	}
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

	void SurfaceFit(float * pose, int motion, float phase);		// definida mais abaixo (precisa dos raios)

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
		if(style == FC_STYLE_WALL) SurfaceFit(g_pose, m, phase);		// maos e pes na superficie real (so nos ciclos da parede)
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

	struct InputState { volatile LONG forward, back, left, right, sprint, jumpEdge, sneakEdge; volatile float ax, ay; };
	InputState g_in;
	bool g_stickDir[4] = { false, false, false, false };	// o analogico esquerdo passado a 'teclas' (frente, tras, esquerda, direita)

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

		// diagnostico das bordas: o que e esta "borda"? (se subir para algo invisivel, esta linha diz que colisao e)
		_MESSAGE("gameplay: ledge accepted - height %.0f above the feet (top z %.1f at %.0f, %.0f), wall distance %.0f, top surface: collision layer %u, %s, form type %u, normal.z %.2f",
				 (double)height, (double)top.hitPosition.z, (double)top.hitPosition.x, (double)top.hitPosition.y, (double)wall.dist,
				 (unsigned)top.layer, top.hasReference ? "belongs to a placed object" : "NO placed object (terrain or loose collision)", (unsigned)top.formType, (double)top.normal.z);
		return true;
	}

	// ------------------------------------------------------------------ passar por cima da borda
	// O personagem MOVE-SE mesmo ao longo do caminho da animacao (a camara e as armas acompanham-no) e, no fim,
	// ja esta em cima da borda: nao ha "salto" de posicao. O desenho do corpo usa so as rotacoes do clipe.
	// Vira o personagem para a parede, com arranque e travagem suaves (TurnSmooth/TurnSpeed no .ini).
	void ApplyFacing(Actor * player)
	{
		if(!g_cfg.faceWall) return;
		const float goal = atan2f(-g_n[0], -g_n[1]);
		float & yaw = Engine::Rotation(player).z;
		if(g_cfg.turnSmooth <= 0.001f) { yaw = goal; g_yawOff = 0.0f; g_yawInit = true; g_prevGoalYaw = goal; g_prevGoalValid = true; return; }

		const float dt = QpcSeconds(&g_qpcFaceLast);
		if(!g_yawInit) { g_yawOff = WrapPi(yaw - goal); g_yawOffVel = 0.0f; g_yawInit = true; }
		else if(g_prevGoalValid)
		{
			const float jump = WrapPi(goal - g_prevGoalYaw);
			if(fabsf(jump) > 0.07f) g_yawOff -= jump;		// a normal deu um salto (canto/faceta): o corpo nao teleporta, desvanece
		}
		g_prevGoalYaw = goal; g_prevGoalValid = true;
		g_yawOff = DecayToZero(g_yawOff, &g_yawOffVel, g_cfg.turnSmooth, g_cfg.turnSpeed * 0.0174533f, dt);
		yaw = WrapPi(goal + g_yawOff);
	}

	// Poe o controlador onde o personagem se MOSTRA: a posicao logica mais o desvio que desvanece.
	void PlaceView(void * ctrl)
	{
		if(g_cfg.posSmooth > 0.001f)
		{
			const float dt = QpcSeconds(&g_qpcViewLast);
			g_off[0] = DecayToZero(g_off[0], &g_offVel[0], g_cfg.posSmooth, 700.0f, dt);
			g_off[1] = DecayToZero(g_off[1], &g_offVel[1], g_cfg.posSmooth, 700.0f, dt);
		}
		else g_off[0] = g_off[1] = 0.0f;
		PlaceController(ctrl, g_pos[0] + g_off[0], g_pos[1] + g_off[1], g_pos[2]);
	}

	void FinishMantle()
	{
		FollowersPlaceAtPlayer();
		FollowersReleaseAll();
		ResetSmooth();
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
		CommitView();
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

		Options::PlaySoundFx(Options::kSndTop);
		StaminaSpend(player, g_cfg.stMantle, true);		// passar a borda nunca e recusado (evita ficar preso); gasta o que houver
		Show(kMantle, 0.0f, 0, FC_STYLE_BODY);
		InterlockedExchange(&g_state, kTopOut);
		_MESSAGE("gameplay: MANTLE over a ledge %.0f units above the feet (wall %.0f ahead)", (double)ledge.height, (double)ledge.wallDistance);
		return true;
	}

	// ------------------------------------------------------------------ agarrar-se a parede
	void Release(const char * why, float pushSpeed = 60.0f)
	{
		FollowersReleaseAll();
		CommitView();
		ResetSmooth();
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
		g_prevContactMotion = 0;
		for(int k = 0; k < 4; ++k) g_fitDelta[k] = 0.0f;
		Show(kHang, 0.0f, 0, FC_STYLE_WALL);
		Options::PlaySoundFx(Options::kSndGrip);
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
		SnapPos(w.x + g_n[0] * g_cfg.gap, w.y + g_n[1] * g_cfg.gap);
		PlaceView(ctrl);
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
		ResetSmooth();
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

		ApplyFacing(player);
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
		Options::PlaySoundFx(Options::kSndPush);
	}

	// ------------------------------------------------------------------ saltos na parede (movimento + salto)
	// 'context' = salto automatico entre paredes: o centro do corpo nao e deslocado pela animacao (o personagem move-se mesmo).
	// Maos e pes tocam a superficie REAL: um raio por membro em contacto (com orcamento por quadro: 4 no maximo, e 1 quando o jogo
	// anda abaixo de FitMinFps), e a diferenca e corrigida com IK. Em arvores, rochas e paredes irregulares cada mao fica na sua superficie.
	void SurfaceFit(float * pose, int motion, float phase)
	{
		if(!g_cfg.fitOn || g_state != kCling || motion < 1 || motion > 5) return;
		Actor * player = ThePlayer();
		if(!player) return;

		float contact[4], pos[12];
		if(!FC_Contacts(motion, phase, contact) || !FC_LimbPositions(pose, pos)) return;

		int budget = g_cfg.fitRays > 4 ? 4 : g_cfg.fitRays;
		if(g_fpsAvg < g_cfg.fitMinFps) budget = 1;

		const float fx = -g_n[0], fy = -g_n[1];			// frente: para a parede
		const float rx = fy, ry = -fx;						// direita
		const float back = 18.0f;
		int done = 0;
		for(int k = 0; k < 4 && done < budget; ++k)
		{
			const int limb = (g_fitRR + k) & 3;
			if(contact[limb] < 0.3f) { g_fitDelta[limb] *= 0.9f; continue; }		// sem contacto: relaxa

			const float lx = pos[limb * 3], ly = pos[limb * 3 + 1], lz = pos[limb * 3 + 2];
			const float wx = g_pos[0] + rx * lx + fx * ly, wy = g_pos[1] + ry * lx + fy * ly, wz = g_pos[2] + lz;
			const Havok::Result r = Havok::RayCast(NiPoint3(wx - fx * back, wy - fy * back, wz), NiPoint3(fx, fy, 0.0f), 50.0f, Havok::kLayer_Transparent, player);
			++done;

			float d = 0.0f;
			if(r.didHit && fabsf(r.normal.z) < 0.6f && r.normal.x * fx + r.normal.y * fy < -0.3f)
				d = (r.distance - back) - (limb < 2 ? 6.0f : 5.0f);			// a palma (ou a ponta do pe) a 5-6 unidades da superficie
			g_fitDelta[limb] += (d - g_fitDelta[limb]) * 0.5f;					// suaviza: cada membro e medido de vez em quando
		}
		g_fitRR = (g_fitRR + (done > 0 ? done : 1)) & 3;
		FC_FitLimbs(pose, g_fitDelta, contact);
	}

	// ---- seguidores (experimental): o NPC escolhido no FreeRun (F11) escala ao lado do jogador, com a MESMA pose ----
	// A FreerunLE.dll entrega-nos o ator por FreeClimb_SetFollower (ver o fim do ficheiro). Aqui o NPC fica preso (sem fisica) ao
	// lado do jogador na parede, virado para ela, e o motor de pose aplica-lhe a pose do jogador (SceneHooks).
	struct FollowerSlot { Actor * actor; UInt32 formId; bool placed; float off[3], offVel[3]; float yaw; };
	FollowerSlot	g_fs[3];

	bool FollowerUsable(Actor * a, UInt32 id)
	{
		__try
		{
			if(!a || !Engine::Readable(a, 0x100)) return false;
			if(((TESForm *)a)->formID != id) return false;
			if(Engine::LifeState(a) != Engine::kLifeState_Alive) return false;
			if(!Engine::ParentCell(a) || !Engine::CharController(a) || !Engine::Get3D(a, false)) return false;
			return true;
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void FollowerRelease(int i)
	{
		FollowerSlot & s = g_fs[i];
		SceneHooksFollowerPose(i, false);
		if(s.placed && FollowerUsable(s.actor, s.formId))
		{
			void * ctrl = Engine::CharController(s.actor);
			if(ctrl)
			{
				Engine::ControllerFlags(ctrl) &= ~Engine::Controller::kFlag_NoSim;
				__declspec(align(16)) float stop[4] = { 0, 0, 0, 0 };
				Engine::ControllerSetLinearVelocity(ctrl, stop);
			}
		}
		s.placed = false;
	}

	void FollowersReleaseAll()
	{
		for(int i = 0; i < 3; ++i) if(g_fs[i].actor) FollowerRelease(i);
	}

	// Cada seguidor fica ao lado do jogador (alternando direita/esquerda), a mesma distancia da parede, virado para ela.
	void FollowerTick(Actor * player, float dt)
	{
		if(!g_cfg.followerOn) { FollowersReleaseAll(); return; }

		static const float chest[1] = { 70.0f };
		for(int i = 0; i < 3; ++i)
		{
			FollowerSlot & s = g_fs[i];
			if(!s.actor) continue;
			if(!FollowerUsable(s.actor, s.formId))
			{
				_MESSAGE("follower %d: no longer usable - forgotten", i);
				SceneHooksUnbindFollower(i);
				s.actor = NULL; s.placed = false;
				continue;
			}

			const float side = ((i & 1) ? -1.0f : 1.0f) * (float)(1 + (i >> 1)) * g_cfg.followerOffset;
			const float rx = -g_n[1], ry = g_n[0];
			const float cx = g_pos[0] + rx * side, cy = g_pos[1] + ry * side;
			const WallHit w = ProbeWall(player, -g_n[0], -g_n[1], cx, cy, g_pos[2], g_cfg.gap + 70.0f, chest, 1);
			if(!w.found || w.nx * g_n[0] + w.ny * g_n[1] < 0.5f)
			{
				if(s.placed) { _MESSAGE("follower %d: no wall beside the player - released", i); FollowerRelease(i); }
				continue;
			}
			const float tx = w.x + w.nx * g_cfg.gap, ty = w.y + w.ny * g_cfg.gap, tz = g_pos[2];

			void * ctrl = Engine::CharController(s.actor);
			if(!ctrl) continue;

			if(!s.placed)
			{
				const NiPoint3 here = Engine::Position(s.actor);
				const float dx = here.x - tx, dy = here.y - ty, dz = here.z - tz;
				if(dx * dx + dy * dy + dz * dz > 220.0f * 220.0f) continue;		// longe demais: nao participa nesta parede

				if(!SceneHooksFollowerBound(i) && !SceneHooksBindFollower(i, s.actor)) continue;
				s.off[0] = dx; s.off[1] = dy; s.off[2] = dz;						// parte de onde esta e chega suavemente
				s.offVel[0] = s.offVel[1] = s.offVel[2] = 0.0f;
				s.yaw = Engine::Rotation(s.actor).z;
				Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;
				s.placed = true;
				_MESSAGE("follower %d: climbing with the player (%.0f units from the wall spot)", i, (double)sqrtf(dx * dx + dy * dy + dz * dz));
			}
			if(!SceneHooksFollowerBound(i) && !SceneHooksBindFollower(i, s.actor)) { FollowerRelease(i); continue; }

			for(int k = 0; k < 3; ++k) s.off[k] = DecayToZero(s.off[k], &s.offVel[k], 0.18f, 700.0f, dt);
			Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;
			PlaceController(ctrl, tx + s.off[0], ty + s.off[1], tz + s.off[2]);

			// vira para a parede com suavidade
			const float goal = atan2f(-w.nx, -w.ny);
			s.yaw += WrapPi(goal - s.yaw) * (1.0f - expf(-dt / 0.12f));
			Engine::Rotation(s.actor).z = WrapPi(s.yaw);
			SceneHooksFollowerPose(i, true);
		}
	}

	// Ao passar a borda: os seguidores aparecem em cima, ao lado do jogador, e voltam a ter fisica.
	void FollowersPlaceAtPlayer()
	{
		Actor * player = ThePlayer();
		if(!player) return;
		const NiPoint3 p = Engine::Position(player);
		for(int i = 0; i < 3; ++i)
		{
			FollowerSlot & s = g_fs[i];
			if(!s.actor || !s.placed || !FollowerUsable(s.actor, s.formId)) continue;
			void * ctrl = Engine::CharController(s.actor);
			if(!ctrl) continue;
			const float side = ((i & 1) ? -1.0f : 1.0f) * (float)(1 + (i >> 1)) * 45.0f;
			const float rx = -g_n[1], ry = g_n[0];
			PlaceController(ctrl, p.x + rx * side - g_n[0] * 40.0f, p.y + ry * side - g_n[1] * 40.0f, p.z);
		}
	}

	// Caminho de um salto que SEGUE a superficie (pilares, arvores): pontos x, y, z e a normal (nx, ny) em cada um.
	enum { kMaxHopPath = 40 };
	float			g_hopPath[kMaxHopPath][5];
	int				g_hopPathN = 0;

	// Anda pela parede em passos, medindo-a a cada passo (por isso curva em volta de pilares e arvores, e para onde a parede acaba).
	// Devolve o numero de pontos (o 0 e o ponto de partida) e quanto se percorreu.
	int PlanHopAlongWall(Actor * player, float sign, float distance, float * reached)
	{
		float step = 20.0f;
		static const float chest[1] = { 70.0f };
		float px = g_pos[0], py = g_pos[1], nx = g_n[0], ny = g_n[1], travelled = 0.0f;
		g_hopPath[0][0] = px; g_hopPath[0][1] = py; g_hopPath[0][2] = g_pos[2]; g_hopPath[0][3] = nx; g_hopPath[0][4] = ny;
		int n = 1;

		for(int guard = 0; guard < 80 && travelled < distance - 0.5f && n < kMaxHopPath; ++guard)
		{
			const float s = (distance - travelled) < step ? (distance - travelled) : step;
			const float cx = px + (-ny * sign) * s, cy = py + (nx * sign) * s;			// um passo ao longo da tangente
			const WallHit w = ProbeWall(player, -nx, -ny, cx, cy, g_pos[2], g_cfg.gap + step + 25.0f, chest, 1);

			bool ok = w.found && (w.nx * nx + w.ny * ny >= 0.5f);							// senao: acabou, ou e um canto (mais de ~60 graus)
			float npx = 0.0f, npy = 0.0f;
			if(ok)
			{
				npx = w.x + w.nx * g_cfg.gap; npy = w.y + w.ny * g_cfg.gap;
				const float jx = npx - cx, jy = npy - cy;
				if(sqrtf(jx * jx + jy * jy) > s + 25.0f) ok = false;						// salto brusco: outro objeto
			}
			if(!ok)
			{
				// um passo grande pode passar por cima de um pilar fino ou de uma aresta: tenta com passos mais pequenos
				if(step > 5.0f) { step *= 0.5f; continue; }
				break;
			}
			px = npx; py = npy; nx = w.nx; ny = w.ny; travelled += s;
			g_hopPath[n][0] = px; g_hopPath[n][1] = py; g_hopPath[n][2] = g_pos[2]; g_hopPath[n][3] = nx; g_hopPath[n][4] = ny;
			++n;
		}
		*reached = travelled;
		return n;
	}

	// Salto sem parede a frente: sai como um salto normal, na direcao pedida, e depois cai (nao tenta agarrar nada no ar).
	void StartFailedHop(int motion, float sign, float distance)
	{
		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		const float dx = -g_n[1] * sign, dy = g_n[0] * sign, ox = g_n[0], oy = g_n[1];
		const float clip = FC_MotionSeconds(motion);
		float secs = clip / (g_cfg.hopSpeed > 0.1f ? g_cfg.hopSpeed : 1.0f);
		if(secs < 0.2f) secs = 0.2f;

		float vh = distance / secs;						// a mesma distancia que o salto teria
		if(vh > 420.0f) vh = 420.0f;
		if(vh < 120.0f) vh = 120.0f;
		float vz = 686.0f * secs * 0.5f;				// impulso para cima: o pico a meio do salto (gravidade ~686 u/s2)
		if(vz > 300.0f) vz = 300.0f;
		if(vz < 110.0f) vz = 110.0f;

		Release("hop with no wall ahead - normal jump", 0.0f);
		if(ctrl)
		{
			const float sc = Engine::HavokScale();
			__declspec(align(16)) float vel[4] = { (dx * vh + ox * 45.0f) * sc, (dy * vh + oy * 45.0f) * sc, vz * sc, 0.0f };
			Engine::ControllerSetLinearVelocity(ctrl, vel);
		}
		if(clip > 0.0f)
		{
			g_dropMotion = motion; g_dropStart = GetTickCount(); g_dropSecs = clip;
			Show(motion, 0.0f, 0, FC_STYLE_BODY);
		}
		Options::PlaySoundFx(Options::kSndPush);
		_MESSAGE("gameplay: HOP with no wall ahead - normal jump (%s, %.0f units)", sign < 0.0f ? "left" : "right", (double)distance);
	}

	void StartHop(int motion, float lateral, float vertical, bool context = false)
	{
		CommitView();
		if(!StaminaSpend(ThePlayer(), g_cfg.stHop, false)) return;		// sem estamina nao ha salto
		const float secs = FC_MotionSeconds(motion);
		if(secs <= 0.0f) { Release("hop animation not available"); return; }

		g_hopPathN = 0;
		if(g_cfg.hopProbe && !context && motion != kHopUp && fabsf(lateral) > 0.5f)
		{
			const float want = fabsf(lateral) * g_cfg.hopScale, sign = lateral < 0.0f ? -1.0f : 1.0f;
			float reached = 0.0f;
			const int n = PlanHopAlongWall(ThePlayer(), sign, want, &reached);
			if(n < 2 || reached < (want * 0.3f > 30.0f ? want * 0.3f : 30.0f)) { StartFailedHop(motion, sign, want); return; }

			g_hopPathN = n;
			g_hopFrom[0] = g_hopPath[0][0]; g_hopFrom[1] = g_hopPath[0][1]; g_hopFrom[2] = g_hopPath[0][2];
			g_hopTo[0] = g_hopPath[n - 1][0]; g_hopTo[1] = g_hopPath[n - 1][1]; g_hopTo[2] = g_hopPath[n - 1][2];
			g_hopMotion = motion;
			g_hopCom = 1.0f;
			const float frac = reached / want;
			g_hopSecs = secs / (g_cfg.hopSpeed > 0.1f ? g_cfg.hopSpeed : 1.0f) * (frac < 0.5f ? 0.5f : frac);
			g_hopStart = g_lastTick = GetTickCount();
			Show(motion, 0.0f, 0, FC_STYLE_WALL);
			Options::PlaySoundFx(Options::kSndPush);
			InterlockedExchange(&g_state, kHop);
			_MESSAGE("gameplay: HOP (%s) along the wall - %.0f of %.0f units, %d points", sign < 0.0f ? "left" : "right", (double)reached, (double)want, n);
			return;
		}

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
		Options::PlaySoundFx(Options::kSndPush);
		InterlockedExchange(&g_state, kHop);
		_MESSAGE("gameplay: HOP (%s%s) %.0f units", context ? "automatic, " : "", motion == kHopUp ? "up" : (lateral < 0.0f ? "left" : "right"),
				 (double)(fabsf(lateral) + fabsf(vertical)) * g_cfg.hopScale);
	}

	void TickHop(Actor * player, void * ctrl, DWORD now)
	{
		float p = (float)(now - g_hopStart) / 1000.0f / g_hopSecs;
		if(p > 1.0f) p = 1.0f;
		const float e = g_cfg.hopEase ? p * p * (3.0f - 2.0f * p) : p;			// suave no inicio e no fim (ou constante)

		if(g_hopPathN >= 2)
		{
			// segue a superficie: posicao e normal interpoladas ao longo dos pontos do caminho
			const float f = e * (float)(g_hopPathN - 1);
			int i0 = (int)f;
			if(i0 > g_hopPathN - 2) i0 = g_hopPathN - 2;
			const float t = f - (float)i0;
			const float * a = g_hopPath[i0], * b = g_hopPath[i0 + 1];
			for(int k = 0; k < 3; ++k) g_pos[k] = a[k] + (b[k] - a[k]) * t;
			float nx = a[3] + (b[3] - a[3]) * t, ny = a[4] + (b[4] - a[4]) * t;
			const float len = sqrtf(nx * nx + ny * ny);
			if(len > 0.01f) { g_n[0] = nx / len; g_n[1] = ny / len; }
		}
		else for(int k = 0; k < 3; ++k) g_pos[k] = g_hopFrom[k] + (g_hopTo[k] - g_hopFrom[k]) * e;
		ApplyFacing(player);
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
					Options::ReloadIfChanged();
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
		const float frameDt = QpcSeconds(&g_qpcLast);		// relogio de alta resolucao: o GetTickCount tem ~15 ms de grao e faz o movimento tremer
		g_fpsAvg += ((frameDt > 0.0005f ? 1.0f / frameDt : 60.0f) - g_fpsAvg) * 0.05f;
		// estamina: enquanto esta na parede a regeneracao fica suspensa (so aqui: em idle ja saimos acima)
		if(g_cfg.staminaOn && g_cfg.stBlockRegen && !g_exhausted) Engine::UpdateRegenDelay(player, Engine::kActorValue_Stamina, 1.0f);
		if(g_state == kTopOut) { g_lastTick = now; TickMantle(ctrl, now); return; }
		if(g_state == kHop) { g_lastTick = now; TickHop(player, ctrl, now); return; }
		if(g_state == kCatch) { g_lastTick = now; TickCatch(player, ctrl, now); return; }

		const float dt = frameDt;
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
		if(g_cfg.analogOn)
		{
			// analogico: direcao e velocidade continuas (zona morta configuravel; fora dela a magnitude volta a ir de 0 a 1)
			const float ax = g_in.ax, ay = g_in.ay, mag = sqrtf(ax * ax + ay * ay);
			if(mag > g_cfg.analogDead && mag > 0.001f)
			{
				float m = (mag - g_cfg.analogDead) / (1.0f - g_cfg.analogDead);
				if(m > 1.0f) m = 1.0f;
				ix = ax / mag * m; iy = ay / mag * m;
			}
		}
		{ const float len = sqrtf(ix * ix + iy * iy); if(len > 1.0f) { ix /= len; iy /= len; } }
		const bool preMoving = fabsf(ix) + fabsf(iy) > 0.1f;
		const bool preRun = g_in.sprint && iy >= 0.0f && preMoving;

		// estamina: custo por segundo conforme o que esta a fazer; sem estamina nao se sobe, escorrega-se
		float stRate = g_cfg.stHang;
		if(preRun) stRate = g_cfg.stRun;
		else if(preMoving) stRate = fabsf(iy) * (iy > 0.0f ? g_cfg.stClimb : g_cfg.stDown) + fabsf(ix) * g_cfg.stSide;
		const bool sliding = StaminaTick(player, frameDt, stRate);
		if(sliding) { ix = 0.0f; iy = 0.0f; }

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
		// Velocidades: por defeito as do PROPRIO CLIP (distancia por ciclo / duracao), para os pes nao patinarem e a animacao
		// nao tocar acelerada. 0 no .ini = automatico; um valor > 0 forca essa velocidade. SpeedScale multiplica tudo.
		const float climbV = (g_cfg.climbSpeed > 0.0f ? g_cfg.climbSpeed : NaturalSpeed(kUp, 45.2f)) * g_cfg.speedScale;
		const float downV = (g_cfg.downSpeed > 0.0f ? g_cfg.downSpeed : NaturalSpeed(kDown, 45.2f)) * g_cfg.speedScale;
		const float sideV = (g_cfg.sideSpeed > 0.0f ? g_cfg.sideSpeed : NaturalSpeed(kLeft, 36.2f)) * g_cfg.speedScale;
		const float vertical = iy > 0.0f ? climbV : downV;

		// arranque mais lento: ao comecar a mexer parte de StartSpeed e sobe suavemente ate 100% em RampSeconds
		if(moving) g_moveT += dt; else g_moveT = 0.0f;
		float ramp = 1.0f;
		if(g_cfg.rampSeconds > 0.01f && g_cfg.startSpeed < 0.999f)
		{
			float t = g_moveT / g_cfg.rampSeconds;
			t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
			ramp = g_cfg.startSpeed + (1.0f - g_cfg.startSpeed) * (t * t * (3.0f - 2.0f * t));
		}

		float vt = ix * (sideV + (runSp - sideV) * mode) * ramp;
		float vz = iy * (vertical + (runSp - vertical) * mode) * ramp;
		if(sliding) { vt = 0.0f; vz = -g_cfg.stSlideSpeed; }

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

		// sons dos passos: quando uma mao ou um pe passa a tocar na parede durante o ciclo
		if(g_cfg.soundOn && moving && g_showBridge < 0.5f)
		{
			float c[4];
			if(FC_Contacts(motion, g_phase, c))
			{
				if(g_prevContactMotion == motion)
					for(int k = 0; k < 4; ++k)
						if(c[k] >= 0.5f && g_prevContact[k] < 0.5f) { Options::PlaySoundFx(Options::kSndStep); break; }
				for(int k = 0; k < 4; ++k) g_prevContact[k] = c[k];
				g_prevContactMotion = motion;
			}
		}
		else g_prevContactMotion = 0;

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
				SnapPos(side.x + g_n[0] * g_cfg.gap, side.y + g_n[1] * g_cfg.gap);
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
					SnapPos(around.x + g_n[0] * g_cfg.gap, around.y + g_n[1] * g_cfg.gap);
					ApplyFacing(player);
					PlaceView(ctrl);
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
		SnapPos(w.x + g_n[0] * g_cfg.gap, w.y + g_n[1] * g_cfg.gap);

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

		// chegou ao chao a descer (ou a escorregar sem estamina)?
		if(down || sliding)
		{
			const Havok::Result ground = Havok::RayCast(NiPoint3(g_pos[0], g_pos[1], g_pos[2] + 20.0f), NiPoint3(0, 0, -1), 40.0f, Havok::kLayer_Transparent, player);
			if(ground.didHit && ground.normal.z > 0.6f) { Release("reached the ground"); return; }
		}

		ApplyFacing(player);
		PlaceView(ctrl);
		ShowCling();
		FollowerTick(player, dt);
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

		Options::ReloadIfChanged();															// afinacoes relidas: nao precisa de recompilar para afinar

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
				if(e->eventType == InputEvent::kEventType_Thumbstick)
				{
					ThumbstickEvent * stick = (ThumbstickEvent *)e;
					if(stick->keyMask == 0xB)				// analogico esquerdo
					{
						g_in.ax = stick->x; g_in.ay = stick->y;
						if(g_cfg.analogOn)
						{
							// tambem como "teclas", para os saltos (W+A, W+D...) funcionarem com o comando; so mexe quando o analogico muda
							const float th = 0.5f;
							const bool d[4] = { stick->y > th, stick->y < -th, stick->x < -th, stick->x > th };
							if(d[0] != g_stickDir[0]) { g_in.forward = d[0]; g_stickDir[0] = d[0]; }
							if(d[1] != g_stickDir[1]) { g_in.back = d[1]; g_stickDir[1] = d[1]; }
							if(d[2] != g_stickDir[2]) { g_in.left = d[2]; g_stickDir[2] = d[2]; }
							if(d[3] != g_stickDir[3]) { g_in.right = d[3]; g_stickDir[3] = d[3]; }
						}
					}
					continue;
				}
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
	memset((void *)&g_in, 0, sizeof(g_in));

	PatchSlot(kJumpHandler,		kSlot_CanProcess,	(void *)&Hook_JumpCanProcess,	&g_origJumpCan);
	PatchSlot(kMovementHandler,	kSlot_CanProcess,	(void *)&Hook_MoveCanProcess,	&g_origMoveCan);
	PatchSlot(kSneakHandler,	kSlot_CanProcess,	(void *)&Hook_SneakCanProcess,	&g_origSneakCan);
	PatchSlot(kJumpHandler,		kSlot_ProcessButton,(void *)&Hook_JumpButton,		&g_origJumpButton);

	Options::Init();
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

// Estado para o menu: 0 = pacote de animacoes ok, 1 = ossos ligados, 2 = a escalar (0/1), 3 = estamina atual (%)
int GameplayStatus(int what)
{
	switch(what)
	{
		case 0: return FC_BoneCount() ? 1 : 0;
		case 1: return SceneHooksBoundBones();
		case 2: return g_state != kIdle ? 1 : 0;
		case 3: return (int)(g_staminaPct + 0.5f);
	}
	return 0;
}

// ---- chamado pela FreerunLE.dll (o modulo NPC do FreeRun) quando o seguidor escolhido com F11 muda ----
// slot 0..2; 'actor' = NULL limpa o slot. Pode ser chamado de qualquer thread: so guarda o ponteiro e o formID; a validacao e feita no tick.
extern "C" __declspec(dllexport) void FreeClimb_SetFollower(int slot, void * actor, UInt32 formId)
{
	if(slot < 0 || slot > 2) return;
	if(g_fs[slot].actor == (Actor *)actor && g_fs[slot].formId == formId) return;

	if(!actor) { SceneHooksUnbindFollower(slot); FollowerRelease(slot); }
	g_fs[slot].actor = (Actor *)actor;
	g_fs[slot].formId = formId;
	g_fs[slot].placed = false;
	if(g_cfg.verbose || actor) _MESSAGE("follower %d: %s (form %08X)", slot, actor ? "set by the FreeRun module" : "cleared", (unsigned)formId);
}
