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
	};
	Config g_cfg;

	void DefaultConfig()
	{
		g_cfg.reach = 110.0f;	g_cfg.gap = 30.0f;
		g_cfg.climbSpeed = 100.0f;	g_cfg.downSpeed = 78.0f;	g_cfg.sideSpeed = 82.0f;		// os padroes do FreeClimb original
		g_cfg.minLedge = 80.0f;	g_cfg.maxLedge = 180.0f;
		g_cfg.headHeight = 125.0f;
		g_cfg.alwaysCling = 1;
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
		}

		_MESSAGE("gameplay: settings: reach %.0f, wall gap %.0f, speeds up/down/side %.0f/%.0f/%.0f, ledge %.0f-%.0f, head %.0f, alwaysCling %d",
				 (double)g_cfg.reach, (double)g_cfg.gap, (double)g_cfg.climbSpeed, (double)g_cfg.downSpeed, (double)g_cfg.sideSpeed,
				 (double)g_cfg.minLedge, (double)g_cfg.maxLedge, (double)g_cfg.headHeight, g_cfg.alwaysCling);
	}

	// ------------------------------------------------------------------ estado
	enum State { kIdle = 0, kCling = 1, kTopOut = 2 };

	// os movimentos do pacote do FreeClimb
	const int	kHang = 1, kUp = 2, kDown = 3, kLeft = 4, kRight = 5, kMantle = 42;
	const float	kUpNatural = 90.3f / 2.0f;			// velocidade natural da animacao "up"/"down" (unidades/s)
	const float	kSideNatural = 42.3f / 1.17f;		// idem "left"/"right"
	const float	kMantleForward = 62.9f, kMantleUp = 132.3f;

	volatile LONG	g_state = kIdle;
	DWORD			g_lastTick = 0, g_attachTime = 0;
	float			g_pos[3];			// pes do personagem enquanto agarrado
	float			g_n[2];				// normal da parede (aponta da parede para o personagem)
	float			g_phase = 0.0f;
	int				g_motion = kHang;
	float			g_destX = 0, g_destY = 0, g_destZ = 0;

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
	// Chamada no fim da animacao (na thread que atualiza o esqueleto): coloca o personagem em cima da borda.
	void OnMantleFinished()
	{
		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(ctrl) PlaceController(ctrl, g_destX, g_destY, g_destZ + 1.0f);
		_MESSAGE("gameplay: climb finished - player placed on top at (%.0f %.0f %.0f)", (double)g_destX, (double)g_destY, (double)g_destZ);
		SceneHooksSetManualPose(false, kHang, 0, 1, 1);
		InterlockedExchange(&g_state, kIdle);
	}

	// Comeca a passar por cima da borda a partir do ponto (px,py,pz), virado para (fx,fy).
	bool StartMantle(Actor * player, float fx, float fy, float px, float py, float pz, const char ** reason)
	{
		Ledge ledge;
		if(!FindLedge(player, fx, fy, px, py, pz, &ledge, reason)) return false;

		const float scaleZ = ledge.height / kMantleUp;
		const float forward = ledge.wallDistance + 40.0f;
		const float scaleY = forward / kMantleForward;
		g_destX = px + fx * forward;
		g_destY = py + fy * forward;
		g_destZ = ledge.topZ;

		SceneHooksSetManualPose(false, kHang, 0, 1, 1);							// a pose em loop da lugar a animacao de uma vez
		if(!SceneHooksPlayMotion(kMantle, scaleY, scaleZ, &OnMantleFinished)) { *reason = "pose engine not ready"; return false; }

		InterlockedExchange(&g_state, kTopOut);
		_MESSAGE("gameplay: MANTLE over a ledge %.0f units above the feet (wall %.0f ahead)", (double)ledge.height, (double)ledge.wallDistance);
		return true;
	}

	// ------------------------------------------------------------------ agarrar-se a parede
	void Release(const char * why)
	{
		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(ctrl)
		{
			__declspec(align(16)) float stop[4] = { 0, 0, 0, 0 };
			Engine::ControllerSetLinearVelocity(ctrl, stop);
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

		g_phase = 0.0f; g_motion = kHang;
		g_attachTime = g_lastTick = GetTickCount();
		g_in.jumpEdge = 0; g_in.sneakEdge = 0;
		SceneHooksSetManualPose(true, kHang, 0.0f, 1.0f, 1.0f);
		InterlockedExchange(&g_state, kCling);

		_MESSAGE("gameplay: CLINGING to the wall (%.0f units ahead). W/S climb, A/D sidestep, Jump or Sneak lets go.", (double)wall.dist);
		return true;
	}

	// A cada atualizacao do esqueleto do jogador (thread de trabalho do jogo).
	void OnTick()
	{
		if(g_state != kCling) return;

		Actor * player = ThePlayer();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(!ctrl) { Release("no controller"); return; }

		const DWORD now = GetTickCount();
		float dt = (float)(now - g_lastTick) / 1000.0f;
		if(dt > 0.1f) dt = 0.1f;
		g_lastTick = now;

		// soltar: salto ou agachar (ignora a pressao que acabou de nos agarrar)
		const LONG jump = InterlockedExchange(&g_in.jumpEdge, 0), sneak = InterlockedExchange(&g_in.sneakEdge, 0);
		if(now - g_attachTime > 400 && (jump || sneak)) { Release(jump ? "jump" : "sneak"); return; }

		const bool up = g_in.forward && !g_in.back, down = g_in.back && !g_in.forward;
		const bool left = g_in.left && !g_in.right, right = g_in.right && !g_in.left;

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
		if(GetTickCount() - g_lastTick > 2500 && g_state == kCling)
		{
			_MESSAGE("gameplay: no skeleton updates for 2.5 s - releasing the controls");
			SceneHooksSetManualPose(false, kHang, 0, 1, 1);
			InterlockedExchange(&g_state, kIdle);
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
