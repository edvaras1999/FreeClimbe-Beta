// PackLoader.h - a unica ligacao entre o plugin (SKSE, C++ antigo) e o nucleo do
// FreeClimb (C++20). So tipos simples: nada de std::filesystem nem de cabecalhos do SKSE.
#pragma once

struct PackSummary
{
	bool		committed;
	unsigned	loaded, rejected, missing;
	unsigned	inputKB, outputKB;
	unsigned	milliseconds;
	char		error[256];
	char		problems[1024];	// "ficheiro: motivo; ..." para o que nao carregou
};

// Carrega o pacote de animacoes (pack.json + configs + .hkx). Nunca lanca excecoes.
bool FC_LoadAnimationPack(const char * manifestPath, PackSummary * out);

// ---- poses de teste da biblioteca carregada (so tipos simples) ----
// Cada pose tem 99 ossos x 10 floats: translacao(3), quaternio x y z w (4), escala(3).
int			FC_BoneCount();				// 99 se a biblioteca esta carregada, senao 0
const char *	FC_BoneName(int bone);		// nome do osso canonico (ex. "NPC L UpperArm [LUar]")
int			FC_PoseCount();
const char *	FC_PoseName(int pose);
bool		FC_GetPose(int pose, float * out990);

// ---- movimentos do pacote tocados ao longo do tempo ----
float		FC_MotionSeconds(int motion);						// duracao real do movimento (0 se nao existe)
bool		FC_SampleMotion(int motion, float phase, float * out990);	// pose na fase 0..1, interpolada

// ---- passo 25: funcoes do original que faltavam (orientacao do corpo na parede) ----
// Distancia (unidades) percorrida num ciclo do movimento; 0 se desconhecida.
float		FC_MotionStride(int motion);

// Como FC_MotionSeconds / FC_SampleMotion, mas escolhendo a sequencia certa de arranque/paragem da corrida
// para a direcao ('direction' = id do movimento de corrida: 20 runUp, 21 runLeft, 22 runRight, 23/24 diagonais; 0 = nenhuma).
float		FC_MotionSecondsDir(int motion, int direction);
bool		FC_SampleMotionDir(int motion, float phase, int direction, float * out990);

// Igual ao "placeWall" do original: poe o corpo a distancia certa da parede (maos sobre a superficie).
// 'feet' = true para as entradas/acoes em que os pes tambem tocam a parede. A posicao do corpo fica no osso 0 (raiz) e no osso 4.
bool		FC_PlaceWall(float * pose990, float gap, float surfaceZ, bool feet);

// Igual ao "placeRun" do original: roda o corpo para a parede (corrida para cima = corpo na horizontal, pes na parede;
// corrida de lado = corpo inclinado, com o braco de dentro apoiado). 'dirX' = esquerda(-)/direita(+), 'dirY' = baixo(-)/cima(+).
// 'reset' = true no primeiro quadro de uma corrida (a orientacao passa a ser suavizada a partir dai).
// O resultado fica na pose: osso 0 = enquadramento do corpo (rotacao + posicao), restantes = rotacoes locais.
bool		FC_PlaceRun(float * pose990, float gait, float dirX, float dirY, float surfaceZ, float gap, float dt, int direction, bool reset);

// Uma chamada que faz tudo: amostra o movimento (com a sequencia certa para a direcao) e coloca o corpo como o original.
//   style 0 = parede (maos na superficie)      1 = parede com os pes (arranques da corrida)
//   style 2 = corrida (rodada para a parede)   3 = o corpo e levado pelo personagem (so rotacoes: apanhar, passar a borda, largar)
//   style 4 = cambalhota para tras
// 'dirX/dirY' e 'dt' so contam na corrida; 'resetRun' = primeiro quadro de uma corrida.
enum { FC_STYLE_WALL = 0, FC_STYLE_WALLFEET = 1, FC_STYLE_RUN = 2, FC_STYLE_BODY = 3, FC_STYLE_BACKFLIP = 4 };
bool		FC_BuildPose(int motion, float phase, int direction, int style, float gap, float dirX, float dirY, float dt, bool resetRun, float * out990);

// Pesos de contacto (0..1) do movimento nesta fase: [mao esq, mao dir, pe esq, pe dir]. Falso se desconhecido.
bool		FC_Contacts(int motion, float phase, float * out4);

// Posicoes (referencial do corpo: x direita, y frente, z cima, a partir dos pes) de [palma esq, palma dir, ponta do pe esq, ponta do pe dir].
bool		FC_LimbPositions(const float * pose990, float * out12);

// Ajusta a pose para os membros chegarem a superficie medida: delta4 = quanto cada membro deve avancar em y (+ para a parede),
// weight4 = pesos de contacto (0..1). Maos e pes, com IK, de forma continua (sem saltos).
bool		FC_FitLimbs(float * pose990, const float * delta4, const float * weight4);
