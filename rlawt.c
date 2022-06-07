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

#include "rlawt.h"
#include <stdlib.h>

static jfieldID AWTContext_instance = 0;
AWTContext *rlawtGetContext(JNIEnv *env, jobject self) {
	if (!AWTContext_instance) {
		jclass clazz = (*env)->GetObjectClass(env, self);
		AWTContext_instance = (*env)->GetFieldID(env, clazz, "instance", "J");
		if (!AWTContext_instance) {
			return NULL;
		}
	}

	jlong ins = (*env)->GetLongField(env, self, AWTContext_instance);
	if (!ins) {
		jclass clazz = (*env)->FindClass(env, "java/lang/NullPointerException");
		(*env)->ThrowNew(env, clazz, "no instance");
	}
	return (AWTContext*) ins;
}

bool rlawtContextState(JNIEnv *env, AWTContext *context, bool created) {
	if (context->contextCreated != created) {
		rlawtThrow(env, created ? "context must be already created" : "context cannot be created");
		return false;
	}
	return true;
}

void rlawtUnlockAWT(JNIEnv *env, AWTContext *ctx) {
	jthrowable exception = (*env)->ExceptionOccurred(env);
	ctx->awt.Unlock(env);
	if (exception) {
		(*env)->Throw(env, exception);
	}
}

JNIEXPORT jlong JNICALL Java_net_runelite_rlawt_AWTContext_create0(JNIEnv *env, jclass _self, jobject component) {
	AWTContext *ctx = calloc(1, sizeof(AWTContext));

	ctx->awt.version = JAWT_VERSION_1_7;
	if (!JAWT_GetAWT(env, &ctx->awt)) {
		rlawtThrow(env, "cannot get the awt");
		goto free_ctx;
	}

	ctx->awt.Lock(env);
	ctx->ds = ctx->awt.GetDrawingSurface(env, component);
	if (!ctx->ds) {
		rlawtThrow(env, "cannot get the ds");
		goto unlock;
	}

	ctx->awt.Unlock(env);

	return (jlong) ctx;

unlock:
	ctx->awt.Unlock(env);
free_ctx:
	free(ctx);
	return 0;
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_destroy(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx) {
		return;
	}

	(*env)->SetLongField(env, self, AWTContext_instance, 0);

	rlawtContextFreePlatform(env, ctx);
	if (ctx->ds) {
		ctx->awt.FreeDrawingSurface(ctx->ds);
	}

	free(ctx);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_configureInsets(JNIEnv *env, jobject self, jint x, jint y) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, false)) {
		return;
	}

	#ifdef __APPLE__
		ctx->offsetX = x;
		ctx->offsetY = y;
	#endif
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_configurePixelFormat(JNIEnv *env, jobject self, jint alpha, jint depth, jint stencil) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, false)) {
		return;
	}

	ctx->alphaDepth = alpha;
	ctx->depthDepth = depth;
	ctx->stencilDepth = stencil;
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_configureMultisamples(JNIEnv *env, jobject self, jint samples) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, false)) {
		return;
	}

	ctx->multisamples = samples;
}

JNIEXPORT jlong JNICALL Java_net_runelite_rlawt_AWTContext_getGLContext(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

	return (jlong) ctx->context;
}

JNIEXPORT jlong JNICALL Java_net_runelite_rlawt_AWTContext_getCGLShareGroup(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

#ifdef __APPLE__
	return (jlong) CGLGetShareGroup(ctx->context);
#else
	rlawtThrow(env, "not supported");
	return 0;
#endif
}

JNIEXPORT jlong JNICALL Java_net_runelite_rlawt_AWTContext_getGLXDisplay(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

#ifdef __unix__
	return (jlong) ctx->dpy;
#else
	rlawtThrow(env, "not supported");
	return 0;
#endif
}

JNIEXPORT jlong JNICALL Java_net_runelite_rlawt_AWTContext_getWGLHDC(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

#ifdef _WIN32
	return (jlong) ctx->dspi->hdc;
#else
	rlawtThrow(env, "not supported");
	return 0;
#endif
}

#ifndef __APPLE__
JNIEXPORT jint JNICALL Java_net_runelite_rlawt_AWTContext_getFramebuffer(JNIEnv *env, jobject self, jboolean front) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

	return 0;
}
#endif