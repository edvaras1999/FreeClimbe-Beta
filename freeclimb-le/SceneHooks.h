// SceneHooks.h - passo 8: interceptadores de PASSAGEM nas funcoes de atualizacao da cena.
// Nao alteram nada: contam as chamadas e chamam a funcao original com os mesmos argumentos.
#pragma once

// Instala os 16 interceptadores (vtables de NiNode, BSFadeNode, BSFlattenedBoneTree, NiAVObject).
void InstallSceneHooks();

// Diz quais sao o no raiz e a tabela achatada do jogador (para contar so as chamadas dele).
void SceneHooksSetTarget(unsigned root, unsigned flat);

// Arranca uma thread que, durante ~40 s, regista no log quantas chamadas houve por segundo.
void SceneHooksStartReport();

// ---- passo 25: motor de pose novo ----
// Afinacoes: poseMode 1 = escrever os locais ANTES de cada passagem (como o original; padrao),
// 0 = escrever o mundo DEPOIS da passagem (modo antigo, so para comparar). entryBlend/exitBlend em segundos.
void SceneHooksConfigure(int poseMode, float entryBlend, float exitBlend);

// Liga os ossos canonicos (99 posicoes: indice da entrada da tabela achatada, -1 = nenhuma).
void SceneHooksBindBones(const int * entryIndex);

// O jogo entrega, a cada atualizacao, a pose FINAL de 99 ossos x 10 floats (osso 0 = enquadramento do corpo).
// 'key' identifica o movimento: quando muda, faz-se uma fusao de 'blendSeconds' da pose anterior para a nova.
// active=false: a ultima pose desvanece-se para a do motor.
void SceneHooksSetPose(bool active, const float * pose990, int key, float blendSeconds);

bool SceneHooksIsPlaying();			// ha uma pose a ser aplicada (incluindo o desvanecimento)
bool SceneHooksPoseFaulted();		// o esqueleto mudou por baixo de nos: o jogo deve largar a parede (le e limpa)

// Direcao para onde o corpo do jogador olha (plana, unitaria), lida da rotacao do no raiz.
bool SceneHooksRootForward(float * x, float * y);

// Chamada uma vez por quadro, na passagem "para baixo" do esqueleto do jogador (antes de a pose ser aplicada).
typedef void (*SceneTickCallback)();
void SceneHooksSetTick(SceneTickCallback cb);

// Volta a procurar a raiz e a tabela achatada do jogador e a ligar os ossos (o jogo reconstroi-o ao equipar, em combate, ao mudar de celula).
bool SceneHooksRefreshTarget();

// (mantida por compatibilidade; o teste de poses do passo 17 foi retirado)
void SceneHooksStartPoseTest();

// Quantos dos 99 ossos canonicos estao ligados ao esqueleto do jogador (para o menu).
int SceneHooksBoundBones();

// ---- seguidores: a pose do jogador aplicada ao esqueleto de outros atores (slots 0..2) ----
bool SceneHooksBindFollower(int slot, void * actor);	// procura a tabela achatada do ator e liga os ossos
void SceneHooksUnbindFollower(int slot);
void SceneHooksFollowerPose(int slot, bool on);		// aplicar (ou nao) a pose do jogador a este seguidor
bool SceneHooksFollowerBound(int slot);
