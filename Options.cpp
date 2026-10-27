// Options.cpp - ver Options.h.
#include "skse/PapyrusVM.h"
#include "skse/PapyrusNativeFunctions.h"
#include "skse/PapyrusEvents.h"
#include "skse/PluginAPI.h"
#include "Log.h"
#include "Options.h"
#include "SceneHooks.h"

#include <string>
#include <string.h>
#include <stdlib.h>
#include <math.h>

FCConfig g_cfg;

namespace
{
	const char * const kIniName		= "FreeClimbLE.ini";
	const char * const kIniFallback	= "Data\\SKSE\\Plugins\\FreeClimbLE.ini";
	const char * const kSection		= "FreeClimb";

	// key == NULL: cabecalho de grupo (so para o .ini escrito pelo menu); 'comment' e a linha de explicacao.
	struct Opt { const char * key; char type; void * ptr; float def, lo, hi; const char * comment; };

	Opt g_opts[] =
	{
		{ NULL, 0, NULL, 0, 0, 0, "ESCALADA" },
		{ "Reach",			'f', &g_cfg.reach,			110.0f,	30.0f,	300.0f,	"Ate onde procura a parede quando carrega em saltar (alcance)." },
		{ "WallGap",		'f', &g_cfg.gap,			30.0f,	10.0f,	80.0f,	"Distancia do personagem a parede enquanto agarrado (30 e a do mod original)." },
		{ "AttachReach",	'f', &g_cfg.attachReach,	65.0f,	10.0f,	200.0f,	"Distancia maxima a parede para se agarrar (o personagem e puxado ate ela)." },
		{ "AirReach",		'f', &g_cfg.airReach,		90.0f,	10.0f,	300.0f,	"Alcance para apanhar uma parede a meio de um salto." },
		{ "AlwaysCling",	'i', &g_cfg.alwaysCling,	1.0f,	0.0f,	1.0f,	"1 = o salto junto a uma parede agarra-se sempre; 0 = passa por cima se for baixa." },
		{ "EntryMode",		'i', &g_cfg.entryMode,		0.0f,	0.0f,	1.0f,	"0 = Salto sozinho junto a uma parede; 1 = Salto + W + A + D juntos (como o original)." },
		{ "FaceWall",		'i', &g_cfg.faceWall,		1.0f,	0.0f,	1.0f,	"1 = vira o personagem de frente para a parede." },
		{ "MinLedge",		'f', &g_cfg.minLedge,		80.0f,	20.0f,	300.0f,	"Altura minima (acima dos pes) de uma borda a passar por cima." },
		{ "MaxLedge",		'f', &g_cfg.maxLedge,		180.0f,	40.0f,	400.0f,	"Altura maxima de uma borda a passar por cima." },
		{ "HeadHeight",		'f', &g_cfg.headHeight,		125.0f,	60.0f,	250.0f,	"Altura da cabeca: onde se procura o topo da parede." },
		{ "Warp",			'i', &g_cfg.warp,			1.0f,	0.0f,	1.0f,	"1 = o controlador e teletransportado a cada atualizacao." },

		{ NULL, 0, NULL, 0, 0, 0, "VELOCIDADE (0 = automatico: a velocidade do proprio clip)" },
		{ "ClimbSpeed",		'f', &g_cfg.climbSpeed,		0.0f,	0.0f,	300.0f,	"Subir (unidades por segundo). O original usa 100." },
		{ "DownSpeed",		'f', &g_cfg.downSpeed,		0.0f,	0.0f,	300.0f,	"Descer. O original usa 78." },
		{ "SideSpeed",		'f', &g_cfg.sideSpeed,		0.0f,	0.0f,	300.0f,	"Andar de lado. O original usa 82." },
		{ "SpeedScale",		'f', &g_cfg.speedScale,		1.0f,	0.25f,	3.0f,	"Multiplica todas as velocidades automaticas." },
		{ "StartSpeed",		'f', &g_cfg.startSpeed,		0.45f,	0.05f,	1.0f,	"Fracao da velocidade ao comecar a mexer (1 = sem rampa)." },
		{ "RampSeconds",	'f', &g_cfg.rampSeconds,		0.7f,	0.0f,	3.0f,	"Segundos ate a velocidade completa." },
		{ "WallRunSpeed",	'f', &g_cfg.wallRunSpeed,	379.5f,	100.0f,	800.0f,	"Corrida na parede (Shift)." },
		{ "WallRunDiagonal",'f', &g_cfg.wallRunDiag,	1.15f,	0.5f,	2.0f,	"Multiplicador da corrida na diagonal." },

		{ NULL, 0, NULL, 0, 0, 0, "SALTOS" },
		{ "HopScale",		'f', &g_cfg.hopScale,		1.8f,	0.5f,	4.0f,	"Multiplica a distancia dos saltos na parede." },
		{ "HopSpeed",		'f', &g_cfg.hopSpeed,		1.1f,	0.5f,	3.0f,	"Multiplica a velocidade dos saltos." },
		{ "HopEase",		'i', &g_cfg.hopEase,		1.0f,	0.0f,	1.0f,	"1 = salto suave (comeca e acaba devagar)." },
		{ "HopProbe",		'i', &g_cfg.hopProbe,		1.0f,	0.0f,	1.0f,	"1 = mede a parede no destino; sem parede o salto sai normal e o personagem cai." },
		{ "ContextHops",	'i', &g_cfg.contextHops,	1.0f,	0.0f,	1.0f,	"1 = salta sozinho para a parede seguinte quando a atual acaba." },
		{ "CatchSpeed",		'f', &g_cfg.catchSpeed,		1.5f,	0.5f,	3.0f,	"Velocidade da animacao de apanhar a parede." },

		{ NULL, 0, NULL, 0, 0, 0, "SUAVIDADE E MOTOR DE POSE" },
		{ "TurnSmooth",		'f', &g_cfg.turnSmooth,		0.15f,	0.0f,	0.6f,	"Suavidade da rotacao do corpo para a parede, em segundos (0 = instantaneo)." },
		{ "TurnSpeed",		'f', &g_cfg.turnSpeed,		540.0f,	60.0f,	1440.0f,	"Velocidade maxima de rotacao (graus por segundo)." },
		{ "PosSmooth",		'f', &g_cfg.posSmooth,		0.10f,	0.0f,	0.5f,	"Suavidade da posicao ao mudar de parede, em segundos." },
		{ "PoseMode",		'i', &g_cfg.poseMode,		1.0f,	0.0f,	1.0f,	"1 = pose escrita antes da passagem do jogo (recomendado); 0 = modo antigo." },
		{ "EntryBlend",		'f', &g_cfg.entryBlend,		0.14f,	0.0f,	1.0f,	"Fusao ao entrar na pose da escalada (segundos)." },
		{ "ExitBlend",		'f', &g_cfg.exitBlend,		0.22f,	0.0f,	1.5f,	"Fusao ao sair da pose (segundos)." },
		{ "MotionBlend",	'f', &g_cfg.motionBlend,	0.15f,	0.0f,	1.0f,	"Fusao ao trocar de animacao (segundos)." },
		{ "RunHeight",		'f', &g_cfg.runHeight,		0.0f,	-80.0f,	120.0f,	"Altura do corpo na corrida na parede (unidades)." },

		{ NULL, 0, NULL, 0, 0, 0, "ESTAMINA (percentagem da estamina maxima)" },
		{ "StaminaEnabled",	'i', &g_cfg.staminaOn,		1.0f,	0.0f,	1.0f,	"Liga a estamina da escalada." },
		{ "StaminaHang",	'f', &g_cfg.stHang,			1.0f,	0.0f,	30.0f,	"Custo por segundo agarrado parado." },
		{ "StaminaClimb",	'f', &g_cfg.stClimb,		3.0f,	0.0f,	30.0f,	"Custo por segundo a subir." },
		{ "StaminaDown",	'f', &g_cfg.stDown,			1.5f,	0.0f,	30.0f,	"Custo por segundo a descer." },
		{ "StaminaSide",	'f', &g_cfg.stSide,			3.0f,	0.0f,	30.0f,	"Custo por segundo a andar de lado." },
		{ "StaminaRun",		'f', &g_cfg.stRun,			8.0f,	0.0f,	50.0f,	"Custo por segundo a correr na parede." },
		{ "StaminaHop",		'f', &g_cfg.stHop,			6.0f,	0.0f,	60.0f,	"Custo de cada salto." },
		{ "StaminaMantle",	'f', &g_cfg.stMantle,		4.0f,	0.0f,	60.0f,	"Custo de passar a borda." },
		{ "StaminaScale",	'f', &g_cfg.stScale,		1.0f,	0.1f,	5.0f,	"Multiplicador de dificuldade de todos os custos." },
		{ "StaminaBlockRegen",'i', &g_cfg.stBlockRegen,	1.0f,	0.0f,	1.0f,	"1 = a regeneracao fica suspensa enquanto esta na parede." },
		{ "StaminaSlideSpeed",'f', &g_cfg.stSlideSpeed,	25.0f,	0.0f,	120.0f,	"Sem estamina, escorrega para baixo a esta velocidade (unidades/s)." },
		{ "StaminaResume",	'f', &g_cfg.stResume,		15.0f,	0.0f,	100.0f,	"Percentagem de estamina para voltar a poder subir depois de esgotar." },

		{ NULL, 0, NULL, 0, 0, 0, "SONS" },
		{ "SoundEnabled",	'i', &g_cfg.soundOn,		1.0f,	0.0f,	1.0f,	"Liga os sons da escalada (agarrar, passos, impulso, topo)." },
		{ "SoundVolume",	'f', &g_cfg.soundVolume,	1.0f,	0.0f,	1.0f,	"Volume dos sons da escalada." },
		{ "SoundGapMs",		'f', &g_cfg.soundGapMs,		140.0f,	0.0f,	1000.0f,	"Intervalo minimo entre repeticoes do mesmo som (milissegundos)." },

		{ NULL, 0, NULL, 0, 0, 0, "COMANDO ANALOGICO" },
		{ "AnalogEnabled",	'i', &g_cfg.analogOn,		1.0f,	0.0f,	1.0f,	"Usa o analogico para direcao e velocidade continuas." },
		{ "AnalogDeadZone",	'f', &g_cfg.analogDead,		0.25f,	0.0f,	0.8f,	"Zona morta do analogico (0 a 1)." },

		{ NULL, 0, NULL, 0, 0, 0, "MAOS E PES NA SUPERFICIE (raios)" },
		{ "SurfaceFit",		'i', &g_cfg.fitOn,			1.0f,	0.0f,	1.0f,	"Ajusta maos e pes a superficie real (arvores, rochas, paredes irregulares)." },
		{ "FitRays",		'i', &g_cfg.fitRays,		4.0f,	1.0f,	12.0f,	"Orcamento de raios por atualizacao (mais = mais preciso, mais pesado)." },
		{ "FitMinFps",		'f', &g_cfg.fitMinFps,		35.0f,	10.0f,	120.0f,	"Abaixo destes fps o ajuste usa menos raios sozinho." },

		{ NULL, 0, NULL, 0, 0, 0, "SEGUIDOR (experimental)" },
		{ "FollowerEnabled",'i', &g_cfg.followerOn,		0.0f,	0.0f,	1.0f,	"O seguidor escolhido no FreeRun (F11) escala ao seu lado." },
		{ "FollowerOffset",	'f', &g_cfg.followerOffset,	90.0f,	40.0f,	250.0f,	"Distancia lateral do seguidor a si na parede (unidades)." },

		{ NULL, 0, NULL, 0, 0, 0, "MENU E DIAGNOSTICO" },
		{ "Language",		'i', &g_cfg.language,		0.0f,	0.0f,	1.0f,	"Idioma do menu: 0 = English, 1 = Portugues." },
		{ "Verbose",		'i', &g_cfg.verbose,		0.0f,	0.0f,	1.0f,	"1 = registo detalhado (o log fica grande)." },
	};
	const int kNumOpts = sizeof(g_opts) / sizeof(g_opts[0]);

	char			g_iniPath[MAX_PATH] = { 0 };
	FILETIME		g_iniStamp = { 0, 0 };
	bool			g_loggedPath = false;
	bool			g_inited = false;

	bool FileExists(const char * path)
	{
		const DWORD attrs = GetFileAttributesA(path);
		return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool ReadWholeFile(const char * path, std::string * out)
	{
		HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(file == INVALID_HANDLE_VALUE) return false;
		char buffer[4096];
		DWORD got = 0;
		while(ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got > 0) out->append(buffer, got);
		CloseHandle(file);
		return true;
	}

	bool WriteWholeFile(const char * path, const std::string & text)
	{
		HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if(file == INVALID_HANDLE_VALUE) return false;
		DWORD put = 0;
		const BOOL ok = WriteFile(file, text.data(), (DWORD)text.size(), &put, NULL);
		CloseHandle(file);
		return ok && put == (DWORD)text.size();
	}

	void RefreshStamp()
	{
		WIN32_FILE_ATTRIBUTE_DATA data;
		if(g_iniPath[0] && GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &data)) g_iniStamp = data.ftLastWriteTime;
	}

	bool StampChanged()
	{
		WIN32_FILE_ATTRIBUTE_DATA data;
		if(!g_iniPath[0] || !GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &data)) return false;
		return data.ftLastWriteTime.dwLowDateTime != g_iniStamp.dwLowDateTime || data.ftLastWriteTime.dwHighDateTime != g_iniStamp.dwHighDateTime;
	}

	float Clamp(const Opt * o, float v)
	{
		if(!(v == v)) v = o->def;
		if(v < o->lo) v = o->lo;
		if(v > o->hi) v = o->hi;
		return v;
	}

	void Store(const Opt * o, float v)
	{
		v = Clamp(o, v);
		if(o->type == 'i') *(int *)o->ptr = (int)(v + (v < 0 ? -0.5f : 0.5f));
		else			   *(float *)o->ptr = v;
	}

	float Value(const Opt * o)
	{
		return o->type == 'i' ? (float)*(int *)o->ptr : *(float *)o->ptr;
	}

	const Opt * Find(const char * key)
	{
		if(!key) return NULL;
		for(int i = 0; i < kNumOpts; ++i)
			if(g_opts[i].key && _stricmp(g_opts[i].key, key) == 0) return &g_opts[i];
		return NULL;
	}

	void Defaults(bool keepLanguage)
	{
		for(int i = 0; i < kNumOpts; ++i)
		{
			if(!g_opts[i].key) continue;
			if(keepLanguage && _stricmp(g_opts[i].key, "Language") == 0) continue;
			Store(&g_opts[i], g_opts[i].def);
		}
	}

	// Tras as opcoes que o resto do plugin precisa de conhecer logo.
	void Apply()
	{
		SceneHooksConfigure(g_cfg.poseMode, g_cfg.entryBlend, g_cfg.exitBlend);
	}

	// Le "chave=valor" de um texto (as seccoes e os comentarios sao ignorados). Devolve quantas chaves reconheceu.
	int ParseText(const std::string & text)
	{
		int known = 0;
		size_t pos = 0;
		while(pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if(end == std::string::npos) end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;

			while(!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ' || line[line.size() - 1] == '	')) line.erase(line.size() - 1);
			const size_t start = line.find_first_not_of(" 	");
			if(start == std::string::npos) continue;
			line = line.substr(start);
			if(line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

			const size_t eq = line.find('=');
			if(eq == std::string::npos) continue;

			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			while(!key.empty() && (key[key.size() - 1] == ' ' || key[key.size() - 1] == '	')) key.erase(key.size() - 1);
			const size_t vs = value.find_first_not_of(" 	");
			value = vs == std::string::npos ? "" : value.substr(vs);

			const Opt * o = Find(key.c_str());
			if(!o) continue;
			Store(o, (float)atof(value.c_str()));
			++known;
		}
		return known;
	}

	std::string BuildText(const char * title)
	{
		std::string out;
		out += "; FreeClimbLE.ini - ";
		out += title;
		out += "\r\n; Escrito pelo menu do FreeClimb (MCM) e pelo proprio plugin. Pode editar a mao: e relido\r\n";
		out += "; sozinho na proxima vez que se agarrar a uma parede. Unidades do Skyrim: 1 unidade ~ 1,4 cm.\r\n\r\n[";
		out += kSection;
		out += "]\r\n";

		char buf[64];
		for(int i = 0; i < kNumOpts; ++i)
		{
			const Opt * o = &g_opts[i];
			if(!o->key)
			{
				out += "\r\n; ---- ";
				out += o->comment;
				out += "\r\n";
				continue;
			}
			out += "; ";
			out += o->comment;
			out += "\r\n";
			if(o->type == 'i') _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", *(int *)o->ptr);
			else			   _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.4g", (double)*(float *)o->ptr);
			out += o->key;
			out += "=";
			out += buf;
			out += "\r\n";
		}
		return out;
	}

	bool WriteIni()
	{
		if(!g_iniPath[0]) return false;
		const bool ok = WriteWholeFile(g_iniPath, BuildText("afinacoes da escalada"));
		RefreshStamp();
		if(!ok) _MESSAGE("options: could not write %s", g_iniPath);
		return ok;
	}

	// O .ini fica ao lado da DLL (funciona com o MO2 e o Vortex); se nao houver la, usa Data\SKSE\Plugins;
	// aceita ainda FreeClimb.ini (o nome do mod original) se tiver as nossas chaves.
	void ResolveIniPath()
	{
		g_iniPath[0] = 0;
		char dllDir[MAX_PATH] = { 0 };

		HMODULE self = NULL;
		if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ResolveIniPath, &self))
		{
			char dll[MAX_PATH] = { 0 };
			const DWORD n = GetModuleFileNameA(self, dll, MAX_PATH);
			if(n > 0 && n < MAX_PATH)
			{
				char * slash = strrchr(dll, '\\');
				if(slash) { slash[1] = 0; _snprintf_s(dllDir, sizeof(dllDir), _TRUNCATE, "%s", dll); }
			}
		}

		char candidate[MAX_PATH];
		if(dllDir[0])
		{
			_snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s%s", dllDir, kIniName);
			if(FileExists(candidate)) { _snprintf_s(g_iniPath, sizeof(g_iniPath), _TRUNCATE, "%s", candidate); return; }
		}
		if(FileExists(kIniFallback)) { _snprintf_s(g_iniPath, sizeof(g_iniPath), _TRUNCATE, "%s", kIniFallback); return; }

		if(dllDir[0])
		{
			_snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%sFreeClimb.ini", dllDir);
			std::string text;
			if(ReadWholeFile(candidate, &text) && (text.find("WallGap") != std::string::npos || text.find("StartSpeed") != std::string::npos))
			{
				_snprintf_s(g_iniPath, sizeof(g_iniPath), _TRUNCATE, "%s", candidate);
				return;
			}
		}

		// nenhum: sera criado ao lado da DLL
		if(dllDir[0]) _snprintf_s(g_iniPath, sizeof(g_iniPath), _TRUNCATE, "%s%s", dllDir, kIniName);
		else		  _snprintf_s(g_iniPath, sizeof(g_iniPath), _TRUNCATE, "%s", kIniFallback);
	}

	// ------------------------------------------------------------------ funcoes nativas (menu)
	void SetOpt(const char * key, float value)
	{
		const Opt * o = Find(key);
		if(!o) { _MESSAGE("menu: unknown option '%s'", key ? key : "(null)"); return; }
		Store(o, value);
		_MESSAGE("menu: %s = %g", o->key, (double)Value(o));
		Apply();
		WriteIni();
	}

	bool	N_GetBool(StaticFunctionTag *, BSFixedString key)				{ const Opt * o = Find(key.data); return o ? Value(o) != 0.0f : false; }
	SInt32	N_GetInt(StaticFunctionTag *, BSFixedString key)				{ const Opt * o = Find(key.data); return o ? (SInt32)Value(o) : 0; }
	float	N_GetFloat(StaticFunctionTag *, BSFixedString key)			{ const Opt * o = Find(key.data); return o ? Value(o) : 0.0f; }
	void	N_SetBool(StaticFunctionTag *, BSFixedString key, bool value)	{ SetOpt(key.data, value ? 1.0f : 0.0f); }
	void	N_SetInt(StaticFunctionTag *, BSFixedString key, SInt32 value)	{ SetOpt(key.data, (float)value); }
	void	N_SetFloat(StaticFunctionTag *, BSFixedString key, float value)	{ SetOpt(key.data, value); }
	void	N_Reload(StaticFunctionTag *)										{ Options::Load(); }
	void	N_SaveAll(StaticFunctionTag *)										{ WriteIni(); _MESSAGE("menu: all options saved to the ini"); }

	void N_ResetDefaults(StaticFunctionTag *)
	{
		Defaults(true);
		Apply();
		WriteIni();
		_MESSAGE("menu: defaults restored");
	}

	bool UserIniPath(char * out, size_t n)
	{
		if(!g_iniPath[0]) return false;
		_snprintf_s(out, n, _TRUNCATE, "%s", g_iniPath);
		char * dot = strrchr(out, '.');
		if(!dot) return false;
		_snprintf_s(dot, n - (size_t)(dot - out), _TRUNCATE, "_User.ini");
		return true;
	}

	void N_SaveProfile(StaticFunctionTag *)
	{
		char path[MAX_PATH];
		if(!UserIniPath(path, sizeof(path))) { _MESSAGE("menu: cannot save the user profile (no ini path)"); return; }
		const bool ok = WriteWholeFile(path, BuildText("o seu perfil (guardado pelo menu)"));
		_MESSAGE("menu: user profile %s: %s", ok ? "saved to" : "FAILED to save to", path);
	}

	SInt32 N_LoadProfile(StaticFunctionTag *)
	{
		char path[MAX_PATH];
		std::string text;
		if(!UserIniPath(path, sizeof(path)) || !ReadWholeFile(path, &text)) { _MESSAGE("menu: no saved user profile"); return 0; }
		const int known = ParseText(text);
		Apply();
		WriteIni();
		_MESSAGE("menu: user profile loaded (%d options)", known);
		return known > 0 ? 1 : 0;
	}

	// 0 = pacote de animacoes ok, 1 = ossos ligados, 2 = a escalar, 3 = estamina em %
	SInt32 N_Status(StaticFunctionTag *, SInt32 what)
	{
		return GameplayStatus(what);
	}
}

namespace Options
{
	const char * IniPath() { return g_iniPath; }

	void Load()
	{
		Defaults(true);
		std::string text;
		if(g_iniPath[0] && ReadWholeFile(g_iniPath, &text))
		{
			const int known = ParseText(text);
			if(!g_loggedPath) { g_loggedPath = true; _MESSAGE("options: read %d options from %s", known, g_iniPath); }
		}
		else if(!g_loggedPath) { g_loggedPath = true; _MESSAGE("options: no .ini found - using the defaults (path used when saving: %s)", g_iniPath); }
		RefreshStamp();
		Apply();
	}

	void ReloadIfChanged()
	{
		if(!g_inited) { Init(); return; }
		if(StampChanged())
		{
			Load();
			_MESSAGE("options: the .ini changed - settings reloaded");
		}
	}

	void Init()
	{
		if(g_inited) return;
		g_inited = true;
		ResolveIniPath();
		const bool existed = FileExists(g_iniPath);
		Load();
		if(!existed && WriteIni()) _MESSAGE("options: created %s with the factory values", g_iniPath);
	}

	bool RegisterPapyrus(VMClassRegistry * registry)
	{
		registry->RegisterFunction(new NativeFunction1 <StaticFunctionTag, bool, BSFixedString>("GetBool", "FreeClimbNative", N_GetBool, registry));
		registry->RegisterFunction(new NativeFunction1 <StaticFunctionTag, SInt32, BSFixedString>("GetInt", "FreeClimbNative", N_GetInt, registry));
		registry->RegisterFunction(new NativeFunction1 <StaticFunctionTag, float, BSFixedString>("GetFloat", "FreeClimbNative", N_GetFloat, registry));
		registry->RegisterFunction(new NativeFunction2 <StaticFunctionTag, void, BSFixedString, bool>("SetBool", "FreeClimbNative", N_SetBool, registry));
		registry->RegisterFunction(new NativeFunction2 <StaticFunctionTag, void, BSFixedString, SInt32>("SetInt", "FreeClimbNative", N_SetInt, registry));
		registry->RegisterFunction(new NativeFunction2 <StaticFunctionTag, void, BSFixedString, float>("SetFloat", "FreeClimbNative", N_SetFloat, registry));
		registry->RegisterFunction(new NativeFunction0 <StaticFunctionTag, void>("Reload", "FreeClimbNative", N_Reload, registry));
		registry->RegisterFunction(new NativeFunction0 <StaticFunctionTag, void>("SaveAll", "FreeClimbNative", N_SaveAll, registry));
		registry->RegisterFunction(new NativeFunction0 <StaticFunctionTag, void>("SaveProfile", "FreeClimbNative", N_SaveProfile, registry));
		registry->RegisterFunction(new NativeFunction0 <StaticFunctionTag, SInt32>("LoadProfile", "FreeClimbNative", N_LoadProfile, registry));
		registry->RegisterFunction(new NativeFunction0 <StaticFunctionTag, void>("ResetDefaults", "FreeClimbNative", N_ResetDefaults, registry));
		registry->RegisterFunction(new NativeFunction1 <StaticFunctionTag, SInt32, SInt32>("Status", "FreeClimbNative", N_Status, registry));
		_MESSAGE("papyrus functions registered (FreeClimbNative)");
		return true;
	}

	// ------------------------------------------------------------------ sons (evento "FreeClimb_Sound" para o script do menu)
	SKSEMessagingInterface *					g_messaging = NULL;
	EventDispatcher<SKSEModCallbackEvent> *	g_modEvents = NULL;
	DWORD										g_lastSound[4] = { 0, 0, 0, 0 };

	void SetMessaging(SKSEMessagingInterface * messaging) { g_messaging = messaging; }

	void PlaySoundFx(int sound, TESForm * sender)
	{
		if(sound < 0 || sound > 3 || !g_cfg.soundOn || g_cfg.soundVolume <= 0.001f) return;

		const DWORD now = GetTickCount();
		if(now - g_lastSound[sound] < (DWORD)g_cfg.soundGapMs) return;
		g_lastSound[sound] = now;

		if(!g_modEvents && g_messaging)
			g_modEvents = (EventDispatcher<SKSEModCallbackEvent> *)g_messaging->GetEventDispatcher(SKSEMessagingInterface::kDispatcher_ModEvent);
		if(!g_modEvents) return;

		static const char * const names[4] = { "step", "grip", "push", "top" };
		SKSEModCallbackEvent evn(BSFixedString("FreeClimb_Sound"), BSFixedString(names[sound]), g_cfg.soundVolume, sender);
		g_modEvents->SendEvent(&evn);
		if(g_cfg.verbose) _MESSAGE("sound: %s", names[sound]);
	}
}
