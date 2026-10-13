// SceneHooks.h - passo 8: interceptadores de PASSAGEM nas funcoes de atualizacao da cena.
// Nao alteram nada: contam as chamadas e chamam a funcao original com os mesmos argumentos.
#pragma once

// Instala os 16 interceptadores (vtables de NiNode, BSFadeNode, BSFlattenedBoneTree, NiAVObject).
void InstallSceneHooks();

// Diz quais sao o no raiz e a tabela achatada do jogador (para contar so as chamadas dele).
void SceneHooksSetTarget(unsigned root, unsigned flat);

// Arranca uma thread que, durante ~40 s, regista no log quantas chamadas houve por segundo.
void SceneHooksStartReport();

// Passo 17: aplica ao esqueleto do jogador poses REAIS do pacote de animacoes do FreeClimb.
// 'entryIndex' tem 99 posicoes: o indice da entrada da tabela achatada de cada osso canonico (-1 = nenhum).
void SceneHooksBindBones(const int * entryIndex);

// Depois de o jogador andar e parar, verifica a matematica e aplica 5 poses, uma de cada vez (6 s cada).
void SceneHooksStartPoseTest();

// ---- passo 19: reproduzir uma animacao do FreeClimb a pedido do jogo ----
typedef void (*SceneFinishCallback)();

// Toca um movimento no corpo do jogador. 'scaleY' e 'scaleZ' ajustam o deslocamento do centro do corpo
// (para a frente e para cima) ao tamanho real da borda. 'onFinish' e chamada no fim, na thread que atualiza o esqueleto.
bool SceneHooksPlayMotion(int motion, float scaleY, float scaleZ, SceneFinishCallback onFinish);
bool SceneHooksIsPlaying();

// Direcao para onde o corpo do jogador olha (plana, unitaria), lida da rotacao do no raiz.
bool SceneHooksRootForward(float * x, float * y);

// ---- passo 20: logica de jogo por frame e pose controlada pelo jogo ----
typedef void (*SceneTickCallback)();

// Chamada a cada atualizacao do esqueleto do jogador (antes de desenhar). Corre numa thread de trabalho do jogo.
void SceneHooksSetTick(SceneTickCallback cb);

// Mantem uma pose (movimento + fase 0..1) escrita no corpo a cada frame, ate ser desativada.
void SceneHooksSetManualPose(bool active, int motion, float phase, float scaleY, float scaleZ);

// ---- passo 21: reencontrar o esqueleto do jogador (o jogo reconstroi-o ao equipar, em combate, ao mudar de celula) ----
// Volta a procurar a raiz e a tabela achatada do jogador e a ligar os ossos. Devolve true se ficou utilizavel.
bool SceneHooksRefreshTarget();
