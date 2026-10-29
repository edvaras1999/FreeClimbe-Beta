// FreeClimb LE - passo 1: plugin minimo.
//
// Este ficheiro ainda NAO faz parkour nenhum. Serve para provar tres coisas no
// jogo do utilizador, uma de cada vez:
//   1. o SKSE carrega o plugin e chama as duas funcoes de entrada;
//   2. o plugin escreve no log (Documentos\My Games\Skyrim\SKSE\FreeClimbLE.log);
//   3. o plugin consegue ver os ficheiros de dados do mod FreeClimb e diz se os
//      ficheiros de animacao (.hkx) estao no formato de 64 bits (SE) ou de 32 bits (LE).
//
// O FreeClimb original (SE) e de Eelta, sob a licenca GPL-3.0. Este trabalho
// parte dele e segue a mesma licenca.

#include "skse/PluginAPI.h"
#include "skse/skse_version.h"
#include "Log.h"
#include "PackLoader.h"
#include "RttiReport.h"
#include "SceneHooks.h"
#include "Options.h"
#include "Gameplay.h"

#include <shlobj.h>

#define PLUGIN_NAME				"FreeClimbLE"
#define PLUGIN_VERSION_STRING	"0.29.0 (versao final: escorregar com Shift+S; seguidor desativado)"
#define PLUGIN_LOG_FILE			"\\My Games\\Skyrim\\SKSE\\FreeClimbLE.log"

static PluginHandle				g_pluginHandle	= kPluginHandle_Invalid;
static SKSEMessagingInterface *	g_messaging		= NULL;

namespace
{
	// A pasta das animacoes pode estar no local original do mod (onde o Nemesis a varre)
	// ou, se o utilizador a mover, em Data\SKSE\Plugins\FreeClimb\Animations\ (que o Nemesis ignora).
	const char * const kAnimDirClassic	= "Data\\meshes\\actors\\character\\animations\\FreeClimb\\";
	const char * const kAnimDirPlugin	= "Data\\SKSE\\Plugins\\FreeClimb\\Animations\\";

	bool FileExists(const char * path)
	{
		const DWORD attrs = GetFileAttributesA(path);
		return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	const char * AnimDir()
	{
		static char pluginManifest[MAX_PATH];
		_snprintf_s(pluginManifest, sizeof(pluginManifest), _TRUNCATE, "%spack.json", kAnimDirPlugin);
		return FileExists(pluginManifest) ? kAnimDirPlugin : kAnimDirClassic;
	}

	void ReportFile(const char * path)
	{
		_MESSAGE("  %-72s %s", path, FileExists(path) ? "found" : "NOT FOUND");
	}

	// Cabecalho de um ficheiro .hkx (packfile do Havok): o byte 0x10 diz quantos
	// bytes tem um ponteiro nesse ficheiro. 8 = formato do Skyrim SE, 4 = formato do LE.
	void ReportHkxLayout(const char * path)
	{
		HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
								  FILE_ATTRIBUTE_NORMAL, NULL);
		if(file == INVALID_HANDLE_VALUE)
		{
			_MESSAGE("hkx: could not open %s", path);
			return;
		}

		unsigned char header[0x40] = { 0 };
		DWORD got = 0;
		const BOOL ok = ReadFile(file, header, sizeof(header), &got, NULL);
		CloseHandle(file);

		if(!ok || got < 0x30)
		{
			_MESSAGE("hkx: %s is too short to read (%u bytes)", path, (unsigned)got);
			return;
		}

		const UInt32 magic0 = *(UInt32 *)&header[0x00];
		const UInt32 magic1 = *(UInt32 *)&header[0x04];
		if(magic0 != 0x57E0E057 || magic1 != 0x10C0C010)
		{
			_MESSAGE("hkx: %s is not a Havok packfile (magic %08X %08X)", path, magic0, magic1);
			return;
		}

		char version[17] = { 0 };
		memcpy(version, &header[0x28], 16);

		_MESSAGE("hkx: %s", path);
		_MESSAGE("     packfile version %d, contents '%s', pointer size %u bytes -> %s",
				 *(SInt32 *)&header[0x0C], version, (unsigned)header[0x10],
				 header[0x10] == 8 ? "64-bit layout (Special Edition)" :
				 header[0x10] == 4 ? "32-bit layout (Legendary Edition)" : "unknown layout");
	}

	void CheckModFiles()
	{
		_MESSAGE("looking for the FreeClimb data files (relative to the game folder):");
		ReportFile("Data\\FreeClimb.esp");
		ReportFile("Data\\SKSE\\Plugins\\FreeClimb.ini");

		char path[MAX_PATH];
		const char * const names[] = { "pack.json", "skeleton.json", "configs\\hang.json", "hang.hkx", "up.hkx", "ledgeCatch.hkx" };
		for(int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i)
		{
			_snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s", AnimDir(), names[i]);
			ReportFile(path);
		}

		_snprintf_s(path, sizeof(path), _TRUNCATE, "%shang.hkx", AnimDir());
		ReportHkxLayout(path);
		_snprintf_s(path, sizeof(path), _TRUNCATE, "%sledgeCatch.hkx", AnimDir());
		ReportHkxLayout(path);
	}

	// Passo 2: carrega o pacote de animacoes com o codigo do proprio FreeClimb e diz o resultado.
	void LoadAnimations()
	{
		_MESSAGE("loading the animation pack...");

		char manifest[MAX_PATH];
		_snprintf_s(manifest, sizeof(manifest), _TRUNCATE, "%spack.json", AnimDir());
		_MESSAGE("animation folder: %s", AnimDir());

		PackSummary pack;
		const bool ok = FC_LoadAnimationPack(manifest, &pack);

		_MESSAGE("animation pack: %s - loaded %u, rejected %u, missing %u; read %u KB, decoded %u KB, took %u ms",
				 ok ? "ACCEPTED" : "REJECTED", pack.loaded, pack.rejected, pack.missing,
				 pack.inputKB, pack.outputKB, pack.milliseconds);

		if(pack.error[0])		_MESSAGE("  error: %s", pack.error);
		if(pack.problems[0])	_MESSAGE("  problems: %s", pack.problems);
	}

	void OnMessage(SKSEMessagingInterface::Message * message)
	{
		if(!message) return;

		switch(message->type)
		{
			case SKSEMessagingInterface::kMessage_DataLoaded:
				_MESSAGE("message: DataLoaded - the game's data is ready");
				CheckModFiles();
				LoadAnimations();
				ReportRtti();
				InstallSceneHooks();
				InstallGameplay();
				Options::LogEnvironment();
				break;

			case SKSEMessagingInterface::kMessage_InputLoaded:
				_MESSAGE("message: InputLoaded");
				RegisterGameplayInput();
				break;

			case SKSEMessagingInterface::kMessage_PostLoadGame:
				_MESSAGE("message: a save was loaded");
				ReportScene();
				break;

			case SKSEMessagingInterface::kMessage_NewGame:
				_MESSAGE("message: new game");
				break;

			default:
				break;
		}
	}
}

extern "C"
{

bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);

	_MESSAGE("FreeClimb LE " PLUGIN_VERSION_STRING " (built " __DATE__ " " __TIME__ ")");

	info->infoVersion	= PluginInfo::kInfoVersion;
	info->name			= PLUGIN_NAME;
	info->version		= 1;

	g_pluginHandle = skse->GetPluginHandle();

	if(skse->isEditor)
	{
		_MESSAGE("ABORTING: loaded in the editor");
		return false;
	}

	_MESSAGE("game runtime %08X (1.9.32.0 is %08X)", skse->runtimeVersion, RUNTIME_VERSION_1_9_32_0);

	// A mesma regra do Freerun LE: so o executavel 1.9.32.0 e suportado, porque
	// os enderecos do motor que vamos precisar foram lidos dessa versao.
	if(skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0)
	{
		_MESSAGE("ABORTING: unsupported runtime");
		return false;
	}

	return true;
}

bool SKSEPlugin_Load(const SKSEInterface * skse)
{
	g_messaging = (SKSEMessagingInterface *)skse->QueryInterface(kInterface_Messaging);
	if(!g_messaging || !g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnMessage))
	{
		_MESSAGE("ABORTING: the messaging interface is not available");
		return false;
	}

	Options::SetMessaging(g_messaging);
	Options::Init();

	// o menu (MCM) fala com a DLL por funcoes nativas do Papyrus
	SKSEPapyrusInterface * papyrus = (SKSEPapyrusInterface *)skse->QueryInterface(kInterface_Papyrus);
	if(papyrus && papyrus->Register(Options::RegisterPapyrus)) _MESSAGE("papyrus interface: registered");
	else _MESSAGE("WARNING: could not register the papyrus functions - the in-game menu will not work");

	_MESSAGE("running");
	return true;
}

};
