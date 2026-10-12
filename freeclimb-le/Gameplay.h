// Gameplay.h - passo 19: o primeiro comportamento real do mod.
// Carregar em SALTAR (Espaco) junto a uma borda alcancavel: o plugin deteta a borda com raios do Havok,
// toca a animacao de subida (contextMantle) no corpo do personagem e, no fim, coloca-o em cima da borda.
#pragma once

void InstallGameplay();			// ganchos nos tratadores de teclas e na atualizacao do esqueleto
void RegisterGameplayInput();	// recetor de teclas (chamar quando o jogo avisa que as teclas estao prontas)
