// RttiReport.h - passo 3: descobrir, com o jogo a correr, onde ficam as vtables
// (tabelas de funcoes virtuais) das classes de que o FreeClimb precisa.
#pragma once

// Escreve no log o endereco e o tamanho da vtable de cada classe da lista.
// Nunca fecha o jogo: qualquer falha de leitura de memoria e apanhada e registada.
void ReportRtti();

// Passo 5: le (so leitura) o esqueleto 3D do jogador e regista a sua estrutura:
// classe do no raiz, campos que apontam para objetos, e a arvore de nos com as
// transformacoes. Chamar depois de carregar um save.
void ReportScene();
