// PackLoader.cpp - compilado em C++20, sem cabecalhos do SKSE nem do Windows.
#include "PackLoader.h"
#include "src/AnimationPack.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

// ---------------------------------------------------------------------------------------------
// Passo 25. Funcoes copiadas da logica do SurfacePose do original (placeWall / placeGround / placeRun).
namespace
{
	bool ToPose(const float * in, fc::Pose * pose)
	{
		const fc::Library & lib = *g_library;
		*pose = lib.rest;
		if(pose->size() != 99) return false;
		for(int i = 0; i < 99; ++i)
		{
			const float * o = in + i * 10;
			(*pose)[i].t = fc::Vec{ o[0], o[1], o[2] };
			(*pose)[i].q = fc::Quat{ o[3], o[4], o[5], o[6] };
			(*pose)[i].s = fc::Vec{ o[7], o[8], o[9] };
		}
		return true;
	}

	void FromPose(const fc::Pose & pose, float * out)
	{
		for(int i = 0; i < 99; ++i)
		{
			const fc::Transform & r = pose[i];
			float * o = out + i * 10;
			o[0] = r.t.x; o[1] = r.t.y; o[2] = r.t.z; o[3] = r.q.x; o[4] = r.q.y; o[5] = r.q.z; o[6] = r.q.w;
			o[7] = r.s.x; o[8] = r.s.y; o[9] = r.s.z;
		}
	}

	fc::Quat g_runOrientation;
	fc::Motion Dir(int d) { return (d >= 20 && d <= 24) ? (fc::Motion)d : fc::Motion::none; }
}

float FC_MotionStride(int motion)
{
	if(!FC_BoneCount() || motion < 1 || motion > (int)fc::motionCount) return 0.0f;
	const fc::Clip & clip = g_library->clips[motion - 1];
	return clip.frames.size() >= 2 ? clip.stride : 0.0f;
}

float FC_MotionSecondsDir(int motion, int direction)
{
	try
	{
		if(!FC_BoneCount() || !fc::isActiveMotion((fc::Motion)motion)) return 0.0f;
		const fc::Clip & clip = g_library->clip((fc::Motion)motion, Dir(direction));
		return clip.frames.size() >= 2 ? clip.seconds : 0.0f;
	}
	catch(...) { return 0.0f; }
}

bool FC_SampleMotionDir(int motion, float phase, int direction, float * out)
{
	try
	{
		if(!out || FC_MotionSecondsDir(motion, direction) <= 0.0f) return false;
		if(!(phase == phase)) phase = 0.0f;	// NaN
		const fc::Pose pose = g_library->sample((fc::Motion)motion, phase, Dir(direction));
		if(pose.size() != 99) return false;
		FromPose(pose, out);
		return true;
	}
	catch(...) { return false; }
}

bool FC_PlaceWall(float * pose990, float gap, float surfaceZ, bool feet)
{
	try
	{
		fc::Pose pose;
		if(!pose990 || !FC_BoneCount() || !ToPose(pose990, &pose)) return false;

		const float slope = std::clamp(surfaceZ, 0.0f, 0.68f);
		const fc::Quat tilt = fc::Quat::axis({ 1, 0, 0 }, -std::asin(slope));
		pose[0].q = tilt * pose[0].q;
		pose[0].t = tilt.rotate(pose[0].t);
		if(feet)
		{
			const float h = std::sqrt(1 - slope * slope);
			pose[0].t = pose[0].t + fc::Vec{ 0, h, -slope } * (gap * h - 37.0f);
		}
		else pose[4].t.y += (gap * std::sqrt(1 - slope * slope) - 6 * slope) - 38.0f;

		FromPose(pose, pose990);
		return true;
	}
	catch(...) { return false; }
}

bool FC_PlaceRun(float * pose990, float gait, float dirX, float dirY, float surfaceZ, float gap, float dt, int direction, bool reset)
{
	try
	{
		fc::Pose pose;
		if(!pose990 || !FC_BoneCount() || !ToPose(pose990, &pose)) return false;
		const fc::Library & lib = *g_library;

		if(dt < 0.0f) dt = 0.0f;
		if(dt > 0.05f) dt = 0.05f;
		const float z = std::clamp(std::isfinite(surfaceZ) ? surfaceZ : 0.0f, -0.15f, 0.75f);
		const fc::Vec direction2{ dirX, dirY, 0 };
		const bool sideways = std::fabs(dirX) > 0.1f;
		const bool moving = std::fabs(dirX) + std::fabs(dirY) > 0.1f;
		const fc::Vec dir = moving ? direction2 : fc::Vec{ 0, 1, 0 };

		// orientacao desejada do corpo
		fc::Quat desired;
		if(sideways) desired = fc::sideRunFrame(z, dir).rotation;
		else desired = fc::Quat::axis({ 1, 0, 0 }, std::acos(z)) * fc::Quat::axis({ 0, 0, 1 }, std::atan2(-dir.x, dir.y));
		g_runOrientation = reset ? desired : fc::boundedRotation(g_runOrientation, desired, 4.1887902f * dt);

		const float h = std::sqrt(1 - z * z);
		if(sideways)
		{
			fc::SideRunFrame frame = fc::sideRunFrame(z, dir);
			frame.rotation = g_runOrientation;
			fc::placeSideRun(lib, pose, frame, -gap * h);
			fc::applySideRunBrace(lib, pose, frame, gait, 1.0f, Dir(direction));
		}
		else
		{
			pose[0].q = g_runOrientation * pose[0].q;
			pose[0].t = g_runOrientation.rotate(pose[0].t);
			pose[0].t = pose[0].t + fc::Vec{ 0, h, -z } * (gap * h - 3.0f);
		}

		FromPose(pose, pose990);
		return true;
	}
	catch(...) { return false; }
}

bool FC_BuildPose(int motion, float phase, int direction, int style, float gap, float dirX, float dirY, float dt, bool resetRun, float * out)
{
	try
	{
		if(!out || !FC_SampleMotionDir(motion, phase, direction, out)) return false;
		if(!(gap == gap) || gap < 5.0f) gap = 30.0f;

		switch(style)
		{
			case FC_STYLE_WALL:		return FC_PlaceWall(out, gap, 0.0f, false);
			case FC_STYLE_WALLFEET:	return FC_PlaceWall(out, gap, 0.0f, true);
			case FC_STYLE_RUN:		return FC_PlaceRun(out, phase, dirX, dirY, 0.0f, gap, dt, direction, resetRun);
			case FC_STYLE_BACKFLIP:
			{
				fc::Pose pose;
				if(!ToPose(out, &pose)) return false;
				fc::placeBackFlipOut(*g_library, pose, 0.0f, gap, 1.0f);
				FromPose(pose, out);
				return true;
			}
			default:
			{
				// o personagem leva o corpo: so rotacoes; o deslocamento do centro do corpo e o da raiz ficam no repouso autoral
				fc::Pose pose;
				if(!ToPose(out, &pose)) return false;
				pose[4].t = g_library->rest[4].t;
				pose[0].t = fc::Vec{ 0, 0, 0 };
				pose[0].q = fc::Quat{};
				FromPose(pose, out);
				return true;
			}
		}
	}
	catch(...) { return false; }
}

bool FC_Contacts(int motion, float phase, float * out4)
{
	try
	{
		if(!out4 || !FC_BoneCount() || !fc::isActiveMotion((fc::Motion)motion) || !(phase == phase)) return false;
		const auto w = g_library->contactWeights((fc::Motion)motion, phase);
		for(int i = 0; i < 4; ++i) out4[i] = w[i];
		return true;
	}
	catch(...) { return false; }
}

namespace
{
	float Smooth01(float x) { x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); return x * x * (3.0f - 2.0f * x); }
}

bool FC_LimbPositions(const float * pose990, float * out12)
{
	try
	{
		fc::Pose pose;
		if(!pose990 || !out12 || !FC_BoneCount() || !ToPose(pose990, &pose)) return false;
		const fc::Library & lib = *g_library;
		const auto body = lib.world(pose);
		const fc::Vec p[4] = { lib.palm(body, 0), lib.palm(body, 1), body[50].t, body[51].t };
		for(int i = 0; i < 4; ++i) { out12[i * 3] = p[i].x; out12[i * 3 + 1] = p[i].y; out12[i * 3 + 2] = p[i].z; }
		return true;
	}
	catch(...) { return false; }
}

bool FC_FitLimbs(float * pose990, const float * delta4, const float * weight4)
{
	try
	{
		fc::Pose p;
		if(!pose990 || !delta4 || !weight4 || !FC_BoneCount() || !ToPose(pose990, &p)) return false;
		const fc::Library & lib = *g_library;

		for(int limb = 0; limb < 4; ++limb)
		{
			const bool foot = limb >= 2;
			const int side = limb & 1;
			const float weight = Smooth01((weight4[limb] - 0.15f) / 0.70f);
			float error = delta4[limb];
			if(weight <= 0.01f || !std::isfinite(error)) continue;
			error = std::clamp(error, -10.0f, 10.0f) * weight;
			if(std::fabs(error) < 0.05f) continue;

			const int upper = foot ? (side ? 9 : 6) : (side ? 31 : 28);
			const int mid	= foot ? (side ? 10 : 7) : (side ? 32 : 29);
			const int end	= foot ? (side ? 11 : 8) : (side ? 39 : 38);

			const auto body = lib.world(p);
			fc::Pose candidate = p;
			const fc::Quat localEnd = p[end].q, worldEnd = body[end].q;
			lib.ik(candidate, upper, mid, end, body[end].t + fc::Vec{ 0, error, 0 }, body[mid].t);
			lib.contactOrientation(candidate, end, localEnd, worldEnd);

			// aceitacao continua (sem saltos): so com a articulacao dobrada o suficiente
			const auto checked = lib.world(candidate);
			const float bend = std::acos(std::clamp((checked[mid].t - checked[upper].t).unit().dot((checked[end].t - checked[mid].t).unit()), -1.0f, 1.0f));
			if(!std::isfinite(bend)) continue;
			const float accept = Smooth01((bend - 0.30f) / 0.31f);
			if(accept <= 0.0f) continue;
			for(std::size_t i = 0; i < p.size(); ++i) p[i] = fc::blend(p[i], candidate[i], accept);
		}

		g_library->guardArmBends(p);
		FromPose(p, pose990);
		return true;
	}
	catch(...) { return false; }
}
