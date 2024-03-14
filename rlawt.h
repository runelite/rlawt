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

#include "net_runelite_rlawt_AWTContext.h"
#include <jawt.h>
#include <jawt_md.h>
#include <stdbool.h>

#ifdef __APPLE__
# define GL_SILENCE_DEPRECATION
#	include <IOSurface/IOSurface.h>
# include <QuartzCore/CALayer.h>
# include <OpenGL/OpenGL.h>
#endif

#ifdef __unix__
#	include <X11/Xlib.h>
#	include <GL/glx.h>
#endif

#ifdef _WIN32
#	include <windows.h>
#	include <GL/gl.h>
#	include <wglext.h>
#endif

typedef struct {
	JAWT awt;
	JAWT_DrawingSurface *ds;
	bool contextCreated;

#ifdef __APPLE__
	CALayer *layer;
	IOSurfaceRef buffer[2];
	CGLContextObj context;

	GLuint tex[2];
	GLuint fbo[2];
	int back;

	int offsetX;
	int offsetY;

	GLfloat backingScaleFactor;
#endif

#ifdef __unix__
	Display *dpy;
	Drawable drawable;
	GLXContext context;
	PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
	bool glxSwapControlTear;
	PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
	bool doubleBuffered;
#endif

#ifdef _WIN32
	JAWT_DrawingSurfaceInfo *dsi;
	JAWT_Win32DrawingSurfaceInfo *dspi;
	HGLRC context;
	PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;
	bool wglSwapControlTear;
#endif

	int alphaDepth;
	int depthDepth;
	int stencilDepth;

	int multisamples;
} AWTContext;

void rlawtThrow(JNIEnv *env, const char *msg);
void rlawtUnlockAWT(JNIEnv *env, AWTContext *ctx);
AWTContext *rlawtGetContext(JNIEnv *env, jobject self);
bool rlawtContextState(JNIEnv *env, AWTContext *context, bool created);


void rlawtContextFreePlatform(JNIEnv *env, AWTContext *ctx);