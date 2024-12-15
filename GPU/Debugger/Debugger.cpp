// Copyright (c) 2018- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <vector>
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"
#include "GPU/GPU.h"
#include "GPU/Debugger/Breakpoints.h"
#include "GPU/Debugger/Debugger.h"
#include "GPU/Debugger/Stepping.h"

namespace GPUDebug {

static bool active = false;
static bool inited = false;
static BreakNext breakNext = BreakNext::NONE;
static int breakAtCount = -1;
static bool hasBreakpoints = false;

static int primsLastFrame = 0;
static int primsThisFrame = 0;
static int thisFlipNum = 0;

bool g_primAfterDraw = false;

static double lastStepTime = -1.0;
static uint32_t g_skipPcOnce = 0;

static std::vector<std::pair<int, int>> restrictPrimRanges;
static std::string restrictPrimRule;

const char *BreakNextToString(BreakNext next) {
	switch (next) {
	case BreakNext::NONE: return "NONE,";
	case BreakNext::OP: return "OP";
	case BreakNext::DRAW: return "DRAW";
	case BreakNext::TEX: return "TEX";
	case BreakNext::NONTEX: return "NONTEX";
	case BreakNext::FRAME: return "FRAME";
	case BreakNext::VSYNC: return "VSYNC";
	case BreakNext::PRIM: return "PRIM";
	case BreakNext::CURVE: return "CURVE";
	case BreakNext::COUNT: return "COUNT";
	default: return "N/A";
	}
}

static void Init() {
	if (!inited) {
		GPUBreakpoints::Init([](bool flag) {
			hasBreakpoints = flag;
		});
		inited = true;
	}
}

void SetActive(bool flag) {
	Init();

	active = flag;
	if (!active) {
		breakNext = BreakNext::NONE;
		breakAtCount = -1;
		GPUStepping::ResumeFromStepping();
		lastStepTime = -1.0;
	}
	/*
	active = false;
	breakAtCount = -1;
	hasBreakpoints = false;
	thisFlipNum = 0;
	g_primAfterDraw = false;
	lastStepTime = -1.0f;
	g_skipPcOnce = false;
	*/
}

bool IsActive() {
	return active;
}

BreakNext GetBreakNext() {
	return breakNext;
}

void SetBreakNext(BreakNext next) {
	SetActive(true);
	breakNext = next;
	breakAtCount = -1;
	if (next == BreakNext::TEX) {
		GPUBreakpoints::AddTextureChangeTempBreakpoint();
	} else if (next == BreakNext::PRIM || next == BreakNext::COUNT) {
		GPUBreakpoints::AddCmdBreakpoint(GE_CMD_PRIM, true);
		GPUBreakpoints::AddCmdBreakpoint(GE_CMD_BEZIER, true);
		GPUBreakpoints::AddCmdBreakpoint(GE_CMD_SPLINE, true);
		GPUBreakpoints::AddCmdBreakpoint(GE_CMD_VAP, true);
	} else if (next == BreakNext::CURVE) {
		GPUBreakpoints::AddCmdBreakpoint(GE_CMD_BEZIER, true);
		GPUBreakpoints::AddCmdBreakpoint(GE_CMD_SPLINE, true);
	} else if (next == BreakNext::DRAW) {
		// This is now handled by switching to BreakNext::PRIM when we encounter a flush.
		// This will take us to the following actual draw.
		g_primAfterDraw = true;
	}

	if (GPUStepping::IsStepping()) {
		GPUStepping::ResumeFromStepping();
	}
	lastStepTime = next == BreakNext::NONE ? -1.0 : time_now_d();
}

void SetBreakCount(int c, bool relative) {
	if (relative) {
		breakAtCount = primsThisFrame + c;
	} else {
		breakAtCount = c;
	}
}

NotifyResult NotifyCommand(u32 pc) {
	if (!active) {
		_dbg_assert_(false);
		return NotifyResult::Skip;  // return false
	}

	u32 op = Memory::ReadUnchecked_U32(pc);
	u32 cmd = op >> 24;
	if (thisFlipNum != gpuStats.numFlips) {
		primsLastFrame = primsThisFrame;
		primsThisFrame = 0;
		thisFlipNum = gpuStats.numFlips;
	}

	bool process = true;
	if (cmd == GE_CMD_PRIM || cmd == GE_CMD_BEZIER || cmd == GE_CMD_SPLINE || cmd == GE_CMD_VAP) {
		primsThisFrame++;

		if (!restrictPrimRanges.empty()) {
			process = false;
			for (const auto &range : restrictPrimRanges) {
				if (primsThisFrame >= range.first && primsThisFrame <= range.second) {
					process = true;
					break;
				}
			}
		}
	}

	bool isBreakpoint = false;
	if (breakNext == BreakNext::OP) {
		isBreakpoint = true;
	} else if (breakNext == BreakNext::COUNT) {
		isBreakpoint = primsThisFrame == breakAtCount;
	} else if (hasBreakpoints) {
		isBreakpoint = GPUBreakpoints::IsBreakpoint(pc, op);
	}

	if (isBreakpoint && pc == g_skipPcOnce) {
		INFO_LOG(Log::GeDebugger, "Skipping GE break at %08x (last break was here)", g_skipPcOnce);
		g_skipPcOnce = 0;
		return process ? NotifyResult::Execute : NotifyResult::Skip;
	}
	g_skipPcOnce = 0;

	if (isBreakpoint) {
		GPUBreakpoints::ClearTempBreakpoints();

		if (coreState == CORE_POWERDOWN || !gpuDebug) {
			breakNext = BreakNext::NONE;
			return process ? NotifyResult::Execute : NotifyResult::Skip;
		}

		auto info = gpuDebug->DisassembleOp(pc);
		if (lastStepTime >= 0.0) {
			NOTICE_LOG(Log::GeDebugger, "Waiting at %08x, %s (%fms)", pc, info.desc.c_str(), (time_now_d() - lastStepTime) * 1000.0);
			lastStepTime = -1.0;
		} else {
			NOTICE_LOG(Log::GeDebugger, "Waiting at %08x, %s", pc, info.desc.c_str());
		}

		g_skipPcOnce = pc;
		breakNext = BreakNext::NONE;
		return NotifyResult::Break;  // new. caller will call GPUStepping::EnterStepping().
	}

	return process ? NotifyResult::Execute : NotifyResult::Skip;
}

// TODO: This mechanism is a bit hacky.
void NotifyDraw() {
	if (!active)
		return;
	if (breakNext == BreakNext::DRAW && !GPUStepping::IsStepping()) {
		// Hack to handle draw notifications, that don't come in via NotifyCommand.
		if (g_primAfterDraw) {
			NOTICE_LOG(Log::GeDebugger, "Flush detected, breaking at next PRIM");
			g_primAfterDraw = false;
			// Switch to PRIM mode.
			SetBreakNext(BreakNext::PRIM);
		}
	}
}

void NotifyDisplay(u32 framebuf, u32 stride, int format) {
	if (!active)
		return;
	if (breakNext == BreakNext::FRAME) {
		// This should work fine, start stepping at the first op of the new frame.
		breakNext = BreakNext::OP;
	}
}

void NotifyBeginFrame() {
	if (!active)
		return;
	if (breakNext == BreakNext::VSYNC) {
		// Just start stepping as soon as we can once the vblank finishes.
		breakNext = BreakNext::OP;
	}
}

int PrimsThisFrame() {
	return primsThisFrame;
}

int PrimsLastFrame() {
	return primsLastFrame;
}

static bool ParseRange(const std::string &s, std::pair<int, int> &range) {
	int c = sscanf(s.c_str(), "%d-%d", &range.first, &range.second);
	if (c == 0)
		return false;
	if (c == 1)
		range.second = range.first;
	return true;
}

bool SetRestrictPrims(const char *rule) {
	SetActive(true);
	if (rule == nullptr || rule[0] == 0 || (rule[0] == '*' && rule[1] == 0)) {
		restrictPrimRanges.clear();
		restrictPrimRule.clear();
		return true;
	}

	static constexpr int MAX_PRIMS = 0x7FFFFFFF;
	std::vector<std::string> parts;
	SplitString(rule, ',', parts);

	// Parse expressions like: 0  or  0-1,4-5  or  !2  or  !2-3  or  !2,!3
	std::vector<std::pair<int, int>> updated;
	for (auto &part : parts) {
		std::pair<int, int> range;
		if (part.size() > 1 && part[0] == '!') {
			if (!ParseRange(part.substr(1), range))
				return false;

			// If there's nothing yet, add everything else.
			if (updated.empty()) {
				if (range.first > 0)
					updated.emplace_back(0, range.first - 1);
				if (range.second < MAX_PRIMS)
					updated.emplace_back(range.second + 1, MAX_PRIMS);
				continue;
			}

			// Otherwise, remove this range from any existing.
			for (size_t i = 0; i < updated.size(); ++i) {
				auto &sub = updated[i];
				if (sub.second < range.first || sub.first > range.second)
					continue;
				if (sub.first >= range.first && sub.second <= range.second) {
					// Entire subrange is inside the deleted entries, nuke.
					sub.first = -1;
					sub.second = -1;
					continue;
				}
				if (sub.first < range.first && sub.second > range.second) {
					// We're slicing a hole in this subrange.
					int next = sub.second;
					sub.second = range.first - 1;
					updated.emplace_back(range.second + 1, next);
					continue;
				}

				// If we got here, we're simply clipping the subrange.
				if (sub.first < range.first && sub.second >= range.first && sub.second <= range.second)
					sub.second = range.first - 1;
				if (sub.first >= range.first && sub.first <= range.second && sub.second < range.second)
					sub.first = range.second + 1;
			}
		} else {
			if (!ParseRange(part, range))
				return false;

			updated.push_back(range);
		}
	}

	restrictPrimRanges = updated;
	restrictPrimRule = rule;
	return true;
}

const char *GetRestrictPrims() {
	return restrictPrimRule.c_str();
}

}
