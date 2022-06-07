/*
 * Copyright (c) 2022 Abex
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef _WIN32

#include "rlawt.h"
#include <jawt_md.h>
#include <wingdi.h>

void rlawtThrow(JNIEnv *env, const char *msg) {
	if ((*env)->ExceptionCheck(env)) {
		return;
	}

	jclass clazz = (*env)->FindClass(env, "java/lang/RuntimeException");
	int lastError = GetLastError();
	if (lastError) {
		char buf[256] = {0};
		snprintf(buf, sizeof(buf), "%s (%d)", msg, lastError);
		(*env)->ThrowNew(env, clazz, buf);
	} else {
		(*env)->ThrowNew(env, clazz, msg);
	}
}

static bool makeCurrent(JNIEnv *env, HDC dc, HGLRC context) {
	if (!wglMakeCurrent(dc, context)) {
		rlawtThrow(env, "unable to make current");
		return false;
	}
	return true;
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_createGLContext(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, false)) {
		return;
	}

	jint dsLock = ctx->ds->Lock(ctx->ds);
	if (dsLock & JAWT_LOCK_ERROR) {
		rlawtThrow(env, "unable to lock ds");
		return;
	}

	ctx->dsi = ctx->ds->GetDrawingSurfaceInfo(ctx->ds);
	if (!ctx->dsi) {
		rlawtThrow(env, "unable to get dsi");
		goto unlock;
	}

	ctx->dspi = (JAWT_Win32DrawingSurfaceInfo*) ctx->dsi->platformInfo;
	if (!ctx->dspi || !ctx->dspi->hdc) {
		ctx->ds->FreeDrawingSurfaceInfo(ctx->dsi);
		ctx->dsi = NULL;
		rlawtThrow(env, "unable to get platform dsi");
		goto unlock;
	}

	PIXELFORMATDESCRIPTOR pfd = {0};
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.cRedBits = 8;
	pfd.cBlueBits = 8;
	pfd.cGreenBits = 8;
	pfd.cAlphaBits = ctx->alphaDepth;
	pfd.cDepthBits = ctx->depthDepth;
	pfd.cStencilBits = ctx->stencilDepth;

	int format = ChoosePixelFormat(ctx->dspi->hdc, &pfd);
	if (!format) {
		rlawtThrow(env, "unable to choose format");
		goto unlock;
	}

	if (!SetPixelFormat(ctx->dspi->hdc, format, &pfd)) {
		rlawtThrow(env, "unable to set pixel format");
		goto unlock;
	}

	if (!makeCurrent(env, ctx->dspi->hdc, NULL)) {
		goto unlock;
	}

	ctx->context = wglCreateContext(ctx->dspi->hdc);
	if (!ctx->context) {
		rlawtThrow(env, "unable to create context");
		goto unlock;
	}

	if (!makeCurrent(env, ctx->dspi->hdc, ctx->context)) {
		goto freeContext;
	}

	PFNWGLGETEXTENSIONSSTRINGEXTPROC wglGetExtensionsStringEXT = (PFNWGLGETEXTENSIONSSTRINGEXTPROC) wglGetProcAddress("wglGetExtensionsStringEXT");
	if (wglGetExtensionsStringEXT) {
		const char *extensions = wglGetExtensionsStringEXT();

		if (strstr(extensions, "WGL_EXT_swap_control")) {
			ctx->wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC) wglGetProcAddress("wglSwapIntervalEXT");
			ctx->wglSwapControlTear = !!strstr(extensions, "WGL_EXT_swap_control_tear");
		}
	}

	ctx->ds->Unlock(ctx->ds);

	ctx->contextCreated = true;
	return;

freeContext:
	wglDeleteContext(ctx->context);
unlock:
	jthrowable exception = (*env)->ExceptionOccurred(env);
	ctx->ds->Unlock(ctx->ds);
	if (exception) {
		(*env)->Throw(env, exception);
	}
}

void rlawtContextFreePlatform(JNIEnv *env, AWTContext *ctx) {
	if (ctx->context) {
		wglDeleteContext(ctx->context);
	}
	if (ctx->dsi) {
		ctx->ds->FreeDrawingSurfaceInfo(ctx->dsi);
	}
}

JNIEXPORT jint JNICALL Java_net_runelite_rlawt_AWTContext_setSwapInterval(JNIEnv *env, jobject self, jint interval) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

	ctx->awt.Lock(env);

	if (interval < 0 && !ctx->wglSwapControlTear) {
		interval = -interval;
	}

	if (ctx->wglSwapIntervalEXT) {
		ctx->wglSwapIntervalEXT(interval);
	} else {
		interval = 0;
	}

	rlawtUnlockAWT(env, ctx);
	return interval;
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_makeCurrent(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	makeCurrent(env, ctx->dspi->hdc, ctx->context);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_detachCurrent(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	makeCurrent(env, ctx->dspi->hdc, NULL);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_swapBuffers(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	if (!SwapBuffers(ctx->dspi->hdc)) {
		rlawtThrow(env, "unable to SwapBuffers");
	}
}

#endif