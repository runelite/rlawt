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

#ifdef __unix__

#include "rlawt.h"
#include <jawt_md.h>
#include <string.h>

static XErrorEvent lastError = {0};
static int rlawtXErrorHandler(Display *display, XErrorEvent *event) {
	if (lastError.display == 0) {
		lastError = *event;
	}
	return 0;
}

void rlawtThrow(JNIEnv *env, const char *msg) {
	if ((*env)->ExceptionCheck(env)) {
		return;
	}

	jclass clazz = (*env)->FindClass(env, "java/lang/RuntimeException");
	if (lastError.display) {
		char buf[256] = {0};
		snprintf(buf, sizeof(buf), "%s (glx: %u.%u: %u)", msg, (unsigned) lastError.minor_code, (unsigned) lastError.request_code, (unsigned) lastError.error_code);
		lastError.display = 0;
		(*env)->ThrowNew(env, clazz, buf);
	} else {
		(*env)->ThrowNew(env, clazz, msg);
	}
}

static bool makeCurrent(JNIEnv *env, Display *dpy, GLXDrawable drawable, GLXContext ctx) {
	if (!glXMakeCurrent(dpy, drawable, ctx)) {
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

	ctx->awt.Lock(env);
	XErrorHandler oldErrorHandler = XSetErrorHandler(rlawtXErrorHandler);

	jint dsLock = ctx->ds->Lock(ctx->ds);
	if (dsLock & JAWT_LOCK_ERROR) {
		rlawtThrow(env, "unable to lock ds");
		goto unlock;
	}

	JAWT_DrawingSurfaceInfo *dsi = ctx->ds->GetDrawingSurfaceInfo(ctx->ds);
	if (!dsi) {
		rlawtThrow(env, "unable to get dsi");
		goto unlockDS;
	}

	JAWT_X11DrawingSurfaceInfo *dspi = (JAWT_X11DrawingSurfaceInfo*) dsi->platformInfo;
	if (!dspi || !dspi->display || !dspi->drawable) {
		rlawtThrow(env, "unable to get platform dsi");
		goto freeDSI;
	}

	ctx->drawable = dspi->drawable;

	const char *displayName = XDisplayString(dspi->display);
	ctx->dpy = XOpenDisplay(displayName);
	if (!ctx->dpy) {
		rlawtThrow(env, "unable to open display copy");
		goto freeDSI;
	}

	if (!glXQueryExtension(ctx->dpy, NULL, NULL)) {
		rlawtThrow(env, "glx is not supported");
		goto freeDisplay;
	}

	int screen = DefaultScreen(ctx->dpy);

	GLXFBConfig fbConfig = NULL;
	for (int db = 0; db < 2; db++) {
		ctx->doubleBuffered = db == 0;

		int attribs[] = {
			GLX_RENDER_TYPE, GLX_RGBA_BIT,
			GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, // JAWT never hands out a pixmap
			GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
			GLX_X_RENDERABLE, true,
			GLX_RED_SIZE, 8,
			GLX_GREEN_SIZE, 8,
			GLX_BLUE_SIZE, 8,
			GLX_ALPHA_SIZE, ctx->alphaDepth,
			GLX_DEPTH_SIZE, ctx->depthDepth,
			GLX_STENCIL_SIZE, ctx->stencilDepth,
			GLX_SAMPLE_BUFFERS, ctx->multisamples > 0,
			GLX_SAMPLES, ctx->multisamples,
			GLX_DOUBLEBUFFER, ctx->doubleBuffered,
			None
		};

		int nConfigs;
		GLXFBConfig *fbConfigs = glXChooseFBConfig(ctx->dpy, screen, attribs, &nConfigs);
		if (!fbConfigs) {
			continue;
		}

		// X11 doesn't seem to care if you use a matching visual, but we try to anyway
		for (int i = 0; i < nConfigs; i++) {
			int	fbVid = -1;
			glXGetFBConfigAttrib(ctx->dpy, fbConfigs[i], GLX_VISUAL_ID, &fbVid);
			if (fbVid == dspi->visualID) {
				fbConfig = fbConfigs[i];
				break;
			}
		}

		if (fbConfig) {
			XFree(fbConfigs);
			break;
		} else {
			fbConfig = fbConfigs[0];
			XFree(fbConfigs);
		}
	}
	if (!fbConfig) {
		rlawtThrow(env, "unable to find a fb config");
		goto freeDisplay;
	}

	const char *extensions = glXQueryExtensionsString(ctx->dpy, screen);

	PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB = NULL;
	if (strstr(extensions, "GLX_ARB_create_context")) {
		glXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC) glXGetProcAddressARB("glXCreateContextAttribsARB");
	}

	if (glXCreateContextAttribsARB) {
		int attribs[] = {
			GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
			GLX_CONTEXT_MINOR_VERSION_ARB, 3,
			0
		};
		ctx->context = glXCreateContextAttribsARB(ctx->dpy, fbConfig, NULL, true, attribs);
	} else {
		ctx->context = glXCreateNewContext(ctx->dpy, fbConfig, GLX_RGBA_TYPE, NULL, true);
	}

	if (!ctx->context) {
		rlawtThrow(env, "unable to create glx context");
		goto freeDisplay;
	}

	if (!makeCurrent(env, ctx->dpy, ctx->drawable, ctx->context)) {
		goto freeContext;
	}

	if (strstr(extensions, "GLX_EXT_swap_control")) {
		ctx->glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC) glXGetProcAddress("glXSwapIntervalEXT");
		ctx->glxSwapControlTear = !!strstr(extensions, "GLX_EXT_swap_control_tear");
	} else if (strstr(extensions, "GLX_SGI_swap_control")) {
		ctx->glXSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC) glXGetProcAddress("glXSwapIntervalSGI");
	}

	ctx->ds->FreeDrawingSurfaceInfo(dsi);

	XSync(ctx->dpy, false);
	XSetErrorHandler(oldErrorHandler);
	ctx->ds->Unlock(ctx->ds);
	rlawtUnlockAWT(env, ctx);

	ctx->contextCreated = true;
	return;

freeContext:
	glXDestroyContext(ctx->dpy, ctx->context);
freeDisplay:
	XSync(ctx->dpy, false);
	XCloseDisplay(ctx->dpy);
	jthrowable exception;
freeDSI:
	exception = (*env)->ExceptionOccurred(env);
	ctx->ds->FreeDrawingSurfaceInfo(dsi);
	if (exception) {
		(*env)->Throw(env, exception);
	}
unlockDS:
	exception = (*env)->ExceptionOccurred(env);
	ctx->ds->Unlock(ctx->ds);
	if (exception) {
		(*env)->Throw(env, exception);
	}
unlock:
	XSetErrorHandler(oldErrorHandler);
	rlawtUnlockAWT(env, ctx);
}

void rlawtContextFreePlatform(JNIEnv *env, AWTContext *ctx) {
	if (ctx->contextCreated) {
		glXMakeCurrent(ctx->dpy, None, None);
		glXDestroyContext(ctx->dpy, ctx->context);
		XCloseDisplay(ctx->dpy);
	}
}

JNIEXPORT jint JNICALL Java_net_runelite_rlawt_AWTContext_setSwapInterval(JNIEnv *env, jobject self, jint interval) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

	ctx->awt.Lock(env);

	if (interval < 0 && !ctx->glxSwapControlTear) {
		interval = -interval;
	}
	if (ctx->glXSwapIntervalEXT) {
		ctx->glXSwapIntervalEXT(ctx->dpy, ctx->drawable, interval);
	} else if (ctx->glXSwapIntervalSGI) {
		ctx->glXSwapIntervalSGI(interval);
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

	ctx->awt.Lock(env);

	makeCurrent(env, ctx->dpy, ctx->drawable, ctx->context);

	rlawtUnlockAWT(env, ctx);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_detachCurrent(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	ctx->awt.Lock(env);

	makeCurrent(env, ctx->dpy, None, None);

	rlawtUnlockAWT(env, ctx);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_swapBuffers(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	ctx->awt.Lock(env);
	XErrorHandler oldErrorHandler = XSetErrorHandler(rlawtXErrorHandler);

	if (ctx->doubleBuffered) {
		// TODO: handle -1
		glXSwapBuffers(ctx->dpy, ctx->drawable);
	} else {
		glFinish();
	}

	rlawtUnlockAWT(env, ctx);
}

#endif
