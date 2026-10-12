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
