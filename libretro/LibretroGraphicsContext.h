#pragma once
#include <atomic>

#include "libretro/libretro.h"
#include "Common/GraphicsContext.h"
#include "Common/GPU/thin3d_create.h"

#include "Core/System.h"
#include "GPU/GPUState.h"
#include "GPU/Software/SoftGpu.h"
#include "headless/Compare.h"
#include "Common/Data/Convert/ColorConv.h"

class LibretroGraphicsContext : public GraphicsContext {
public:
	LibretroGraphicsContext() {}
	~LibretroGraphicsContext() override { Shutdown(); }

	virtual bool Init() = 0;
	virtual void SetRenderTarget() {}
	virtual GPUCore GetGPUCore() = 0;
	virtual const char *Ident() = 0;

	void Shutdown() override {
		DestroyDrawContext();
	}
	virtual void SwapBuffers() = 0;
	void Resize() override {}

	virtual void GotBackbuffer();
	virtual void LostBackbuffer();

	virtual void CreateDrawContext() {}
	virtual void DestroyDrawContext() {
		if (!draw_) {
			return;
		}
		delete draw_;
		draw_ = nullptr;
	}
	Draw::DrawContext *GetDrawContext() override { return draw_; }

	static LibretroGraphicsContext *CreateGraphicsContext();

	static retro_video_refresh_t video_cb;

protected:
	Draw::DrawContext *draw_ = nullptr;
};

class LibretroHWRenderContext : public LibretroGraphicsContext {
public:
	LibretroHWRenderContext(retro_hw_context_type context_type, unsigned version_major = 0, unsigned version_minor = 0);
	bool Init(bool cache_context);
	void SetRenderTarget() override {}
	void SwapBuffers() override {
      video_cb(RETRO_HW_FRAME_BUFFER_VALID, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight, 0);
	}
	virtual void ContextReset();
	virtual void ContextDestroy();

protected:
	retro_hw_render_callback hw_render_ = {};
};

#ifdef _WIN32
class LibretroD3D9Context : public LibretroHWRenderContext {
public:
	LibretroD3D9Context() : LibretroHWRenderContext(RETRO_HW_CONTEXT_DIRECT3D, 9) {}
	bool Init() override { return false; }

	void CreateDrawContext() override {
		draw_ = Draw::T3DCreateDX9Context(nullptr, nullptr, 0, nullptr, nullptr);
		draw_->CreatePresets();
	}

	GPUCore GetGPUCore() override { return GPUCORE_DIRECTX9; }
	const char *Ident() override { return "DirectX 9"; }
};
#endif

class LibretroSoftwareContext : public LibretroGraphicsContext {
public:
	LibretroSoftwareContext() {}
	bool Init() override { return true; }
	void SwapBuffers() override {
		GPUDebugBuffer buf;
		u32 w = PSP_CoreParameter().pixelWidth;
		u32 h = PSP_CoreParameter().pixelHeight;
		gpuDebug->GetCurrentFramebuffer(buf, GPU_DBG_FRAMEBUF_DISPLAY);
		const std::vector<u32> pixels = TranslateDebugBufferToCompare(&buf, w, h);
		video_cb(pixels.data(), w, h, w << 2);
    }
	GPUCore GetGPUCore() override { return GPUCORE_SOFTWARE; }
	const char *Ident() override { return "Software"; }
};

namespace Libretro {
extern LibretroGraphicsContext *ctx;
extern retro_environment_t environ_cb;
extern retro_hw_context_type renderer;

enum class EmuThreadState {
	DISABLED,
	START_REQUESTED,
	RUNNING,
	PAUSE_REQUESTED,
	PAUSED,
	QUIT_REQUESTED,
	STOPPED,
};
extern bool useEmuThread;
extern std::atomic<EmuThreadState> emuThreadState;
void EmuThreadStart();
void EmuThreadStop();
void EmuThreadPause();
} // namespace Libretro
