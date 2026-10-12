// PackLoader.cpp - compilado em C++20, sem cabecalhos do SKSE nem do Windows.
#include "PackLoader.h"
#include "src/AnimationPack.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <string>

namespace
{
	fc::Library * g_library = NULL;

	void CopyText(char * dest, size_t size, const std::string & text)
	{
		if(size == 0) return;
		const size_t n = text.size() < size - 1 ? text.size() : size - 1;
		std::memcpy(dest, text.data(), n);
		dest[n] = '\0';
	}
}

bool FC_LoadAnimationPack(const char * manifestPath, PackSummary * out)
{
	if(!out) return false;
	std::memset(out, 0, sizeof(*out));

	try
	{
		// Criada na primeira chamada e nunca destruida: evita ordem de destruicao
		// de globais quando o jogo descarrega a DLL.
		static fc::Library * library = new fc::Library;
		g_library = library;

		const auto start = std::chrono::steady_clock::now();
		const fc::AnimationPackReport report = fc::loadAnimationPack(*library, manifestPath);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

		out->committed		= report.committed;
		out->loaded			= (unsigned)report.loaded;
		out->rejected		= (unsigned)report.rejected;
		out->missing		= (unsigned)report.missing;
		out->inputKB		= (unsigned)(report.inputBytes / 1024);
		out->outputKB		= (unsigned)(report.outputBytes / 1024);
		out->milliseconds	= (unsigned)elapsed.count();
		CopyText(out->error, sizeof(out->error), report.error);

		std::string problems;
		for(const auto & slot : report.slots)
		{
			if(slot.status == fc::OverrideStatus::loaded) continue;
			if(problems.size() > 800) { problems += "..."; break; }
			problems += slot.file + ": " + slot.reason + "; ";
		}
		CopyText(out->problems, sizeof(out->problems), problems);
		return report.committed;
	}
	catch(const std::exception & e)
	{
		CopyText(out->error, sizeof(out->error), std::string("exception: ") + e.what());
	}
	catch(...)
	{
		CopyText(out->error, sizeof(out->error), "unknown exception");
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Poses de teste. A lista: o repouso autoral e alguns quadros reais do pacote de animacoes.
namespace
{
	struct TestPose { const char * name; int motion; int frameMode; };	// frameMode: 0 = primeiro quadro, 1 = quadro do meio
	const TestPose kPoses[] =
	{
		{ "authored rest pose",			0,								0 },
		{ "hang (first frame)",			(int)fc::Motion::hang,			0 },
		{ "contextHang (first frame)",	(int)fc::Motion::contextHang,	0 },
		{ "ledgeCatch (middle frame)",	(int)fc::Motion::ledgeCatch,	1 },
		{ "up (middle frame)",			(int)fc::Motion::up,			1 },
	};
}

int FC_BoneCount()
{
	return (g_library && g_library->names.size() == 99 && g_library->rest.size() == 99) ? 99 : 0;
}

const char * FC_BoneName(int bone)
{
	if(!FC_BoneCount() || bone < 0 || bone >= 99) return "";
	return g_library->names[bone].c_str();
}

int FC_PoseCount()
{
	return FC_BoneCount() ? (int)(sizeof(kPoses) / sizeof(kPoses[0])) : 0;
}

const char * FC_PoseName(int pose)
{
	return (pose >= 0 && pose < (int)(sizeof(kPoses) / sizeof(kPoses[0]))) ? kPoses[pose].name : "";
}

bool FC_GetPose(int pose, float * out)
{
	try
	{
		if(!out || !FC_BoneCount() || pose < 0 || pose >= (int)(sizeof(kPoses) / sizeof(kPoses[0]))) return false;
		const TestPose & tp = kPoses[pose];
		const fc::Library & lib = *g_library;

		if(tp.motion <= 0)
		{
			for(int i = 0; i < 99; ++i)
			{
				const fc::Transform & r = lib.rest[i];
				float * o = out + i * 10;
				o[0] = r.t.x; o[1] = r.t.y; o[2] = r.t.z; o[3] = r.q.x; o[4] = r.q.y; o[5] = r.q.z; o[6] = r.q.w;
				o[7] = r.s.x; o[8] = r.s.y; o[9] = r.s.z;
			}
			return true;
		}

		// quadros reais do pacote: poses completas de 99 ossos
		const fc::Clip & clip = lib.clips[tp.motion - 1];
		if(clip.frames.size() < 2) return false;
		const size_t frame = tp.frameMode == 1 ? clip.frames.size() / 2 : 0;
		const fc::Pose & pose = clip.frames[frame];
		if(pose.size() != 99) return false;

		for(int i = 0; i < 99; ++i)
		{
			const fc::Transform & r = pose[i];
			float * o = out + i * 10;
			o[0] = r.t.x; o[1] = r.t.y; o[2] = r.t.z; o[3] = r.q.x; o[4] = r.q.y; o[5] = r.q.z; o[6] = r.q.w;
			o[7] = r.s.x; o[8] = r.s.y; o[9] = r.s.z;
		}
		return true;
	}
	catch(...) { return false; }
}

float FC_MotionSeconds(int motion)
{
	if(!FC_BoneCount() || motion < 1 || motion > (int)fc::motionCount) return 0.0f;
	const fc::Clip & clip = g_library->clips[motion - 1];
	return clip.frames.size() >= 2 ? clip.seconds : 0.0f;
}

bool FC_SampleMotion(int motion, float phase, float * out)
{
	try
	{
		if(!out || FC_MotionSeconds(motion) <= 0.0f) return false;
		const fc::Pose pose = g_library->sample((fc::Motion)motion, phase);
		if(pose.size() != 99) return false;

		for(int i = 0; i < 99; ++i)
		{
			const fc::Transform & r = pose[i];
			float * o = out + i * 10;
			o[0] = r.t.x; o[1] = r.t.y; o[2] = r.t.z; o[3] = r.q.x; o[4] = r.q.y; o[5] = r.q.z; o[6] = r.q.w;
			o[7] = r.s.x; o[8] = r.s.y; o[9] = r.s.z;
		}
		return true;
	}
	catch(...) { return false; }
}
