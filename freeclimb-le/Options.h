// Options.h - todas as afinacoes do FreeClimb LE numa tabela unica.
//
// A mesma tabela serve para: ler o FreeClimbLE.ini, gravar o .ini, o menu (MCM) por funcoes nativas do
// Papyrus (script FreeClimbNative), o perfil do utilizador e os valores de fabrica. O .ini e gravado a
// cada mudanca no menu, por isso o menu e o .ini nunca divergem.
#pragma once

class VMClassRegistry;
class TESForm;
struct SKSEMessagingInterface;

struct FCConfig
{
	// ---- escalada
	float	reach;			// ate onde procura a parede (unidades)
	float	gap;			// distancia do personagem a parede enquanto agarrado
	float	climbSpeed, downSpeed, sideSpeed;	// velocidades na parede (0 = a do proprio clip)
	float	minLedge, maxLedge;					// bordas a passar por cima (altura acima dos pes)
	float	headHeight;		// altura da cabeca: onde se procura o topo da parede
	int		alwaysCling;	// 1 = o salto junto a uma parede agarra-se sempre
	int		faceWall;		// 1 = vira o personagem de frente para a parede
	float	hopScale;		// multiplica a distancia dos saltos na parede
	float	hopSpeed;		// multiplica a velocidade dos saltos
	float	attachReach;	// distancia maxima a parede para se agarrar
	int		hopEase;		// 1 = salto suave
	int		warp;			// 1 = o controlador e teletransportado a cada frame
	int		entryMode;		// 0 = Salto sozinho; 1 = Salto + W + A + D
	float	airReach;		// alcance para apanhar uma parede a meio de um salto
	float	wallRunSpeed, wallRunDiag;	// corrida na parede (Shift) e multiplicador na diagonal
	int		contextHops;	// 1 = salta sozinho para a parede seguinte
	float	catchSpeed;		// velocidade da animacao de apanhar a parede

	// ---- motor de pose e suavidade
	int		poseMode;		// 1 = pose escrita antes da passagem do motor; 0 = modo antigo
	float	entryBlend, exitBlend, motionBlend;
	float	runHeight;
	float	speedScale, startSpeed, rampSeconds;
	float	turnSmooth, turnSpeed, posSmooth;

	// ---- estamina (percentagem da estamina maxima por segundo, salvo indicacao)
	int		staminaOn;
	float	stHang, stClimb, stDown, stSide, stRun;	// custo por segundo
	float	stHop, stMantle;						// custo fixo
	float	stScale;								// multiplicador de dificuldade
	int		stBlockRegen;							// 1 = a regeneracao fica suspensa na parede
	float	stSlideSpeed;							// a zero de estamina, escorrega a esta velocidade (unidades/s)
	float	stResume;								// so volta a subir quando a estamina passar desta percentagem

	// ---- saltos
	int		hopProbe;		// 1 = mede a parede no destino antes de saltar; sem parede = salto normal

	// ---- sons
	int		soundOn;
	float	soundVolume;	// 0..1
	float	soundGapMs;		// intervalo minimo entre repeticoes do mesmo som

	// ---- comando analogico
	int		analogOn;
	float	analogDead;		// zona morta (0..1)

	// ---- ajuste as superficies reais (raios)
	int		fitOn;
	int		fitRays;		// orcamento de raios por atualizacao
	float	fitMinFps;		// abaixo destes fps o orcamento reduz-se sozinho

	// ---- escorregar (Shift + S)
	int		slideOn;
	float	slideMax;		// velocidade maxima (unidades/s)
	float	slideAccel;		// aceleracao (unidades/s2)
	float	slideStamina;	// custo por segundo (% da estamina maxima)

	// ---- seguidor: DESATIVADO (sem opcao no menu nem no .ini; o codigo fica adormecido, followerOn e sempre 0)
	int		followerOn;
	float	followerOffset;

	// ---- menu / diagnostico
	int		language;		// 0 = English, 1 = Portugues
	int		verbose;		// 1 = log detalhado
};

extern FCConfig g_cfg;

namespace Options
{
	void			Init();							// resolve o caminho do .ini e le (cria um com os valores de fabrica se nao existir)
	void			Load();							// le o .ini outra vez
	void			ReloadIfChanged();				// so le se o ficheiro mudou desde a ultima leitura
	const char *	IniPath();
	bool			RegisterPapyrus(VMClassRegistry * registry);

	// ---- sons: a DLL pede ao menu (script Papyrus) que toque o som; ver FreeClimbMCM.psc
	enum { kSndStep = 0, kSndGrip = 1, kSndPush = 2, kSndTop = 3 };
	void			SetMessaging(SKSEMessagingInterface * messaging);
	void			PlaySoundFx(int sound, TESForm * sender = 0);	// 'sender' = quem faz o som (0 = o jogador)

	// Diagnostico: escreve no log que plugins e formularios do menu o jogo carregou (chamar depois de os dados carregarem).
	void			LogEnvironment();
}

// Implementado em Gameplay.cpp: estado para o menu (0 = pacote de animacoes ok, 1 = ossos ligados, 2 = a escalar, 3 = estamina em %).
int GameplayStatus(int what);
