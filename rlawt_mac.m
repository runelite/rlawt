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

#ifdef __APPLE__

#include "rlawt.h"
#include <jawt_md.h>
#include <OpenGL/gl3.h>
#include <QuartzCore/QuartzCore.h>

@protocol CanSetContentsChanged
-(void)setContentsChanged;
@end

@interface RLLayer : CALayer {
	int offsetY;
}
- (void)setFrame:(CGRect)newValue;
- (void)fixFrame;
@end

@implementation RLLayer
- (void)setFrame:(CGRect)newValue {
	[super setFrame: newValue];

	if (self.superlayer) {
		offsetY = self.superlayer.frame.size.height - newValue.origin.y - newValue.size.height;
	}
}

// JAWT does not update our bounds when the y offset from the bottom of our
// superlayer changes, causing the layer to become displaced from the correct
// vertical position
- (void)fixFrame {
	if (!self.superlayer) {
		return;
	}

	super.frame = CGRectMake(
		self.frame.origin.x,
		self.superlayer.frame.size.height - offsetY - self.frame.size.height,
		self.frame.size.width,
		self.frame.size.height);
}
@end


void rlawtThrow(JNIEnv *env, const char *msg) {
	if ((*env)->ExceptionCheck(env)) {
		return;
	}
	jclass clazz = (*env)->FindClass(env, "java/lang/RuntimeException");
	(*env)->ThrowNew(env, clazz, msg);
}

static void rlawtThrowCGLError(JNIEnv *env, const char *msg, CGLError err) {
	char buf[256] = {0};
	snprintf(buf, sizeof(buf), "%s (cgl: %s)", msg, CGLErrorString(err));
	rlawtThrow(env, buf);
}

static bool makeCurrent(JNIEnv *env, CGLContextObj ctx) {
	CGLError err = CGLSetCurrentContext(ctx);
	if (err != kCGLNoError) {
		rlawtThrowCGLError(env, "unable to make current", err);
		return false;
	}

	return true;
}

static void propsPutInt(CFMutableDictionaryRef props, const CFStringRef key, int value) {
	CFNumberRef boxedValue = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionaryAddValue(props, key, boxedValue);
	CFRelease(boxedValue);
}

static bool rlawtCreateIOSurface(JNIEnv *env, AWTContext *ctx) {
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CGSize size = ctx->layer.frame.size;
	propsPutInt(props, kIOSurfaceHeight, size.height);
	propsPutInt(props, kIOSurfaceWidth, size.width);
	propsPutInt(props, kIOSurfaceBytesPerElement, 4);
	propsPutInt(props, kIOSurfacePixelFormat, (int)'BGRA');

	IOSurfaceRef buf = IOSurfaceCreate(props);
	CFRelease(props);
	if (!buf) {
		rlawtThrow(env, "unable to create io surface");
		return false;
	}
	
	const GLuint target = GL_TEXTURE_RECTANGLE;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, ctx->tex[ctx->back]);
	CGLError err = CGLTexImageIOSurface2D(
		ctx->context,
		target, GL_RGBA,
		size.width, size.height,
		GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
		buf, 
		0);
	glBindTexture(target, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[ctx->back]);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, ctx->tex[ctx->back], 0);

	if (err != kCGLNoError) {
		rlawtThrowCGLError(env, "unable to bind io surface to texture", err);
		goto freeSurface;
	}

	int fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
		char buf[256] = {0};
		snprintf(buf, sizeof(buf), "unable to create fb (%d)", fbStatus);
		rlawtThrow(env, buf);
		goto freeSurface;
	}

	if (ctx->buffer[ctx->back]) {
		CFRelease(ctx->buffer[ctx->back]);
	}
	ctx->buffer[ctx->back] = buf;
	return true;
freeSurface:
	CFRelease(buf);
	return false;
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_createGLContext(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, false)) {
		return;
	}

	JAWT_DrawingSurfaceInfo *dsi = ctx->ds->GetDrawingSurfaceInfo(ctx->ds);
	if (!dsi) {
		rlawtThrow(env, "unable to get dsi");
		return;
	}

	id<JAWT_SurfaceLayers> dspi = (id<JAWT_SurfaceLayers>) dsi->platformInfo;
	if (!dspi) {
		rlawtThrow(env, "unable to get platform dsi");
		goto freeDSI;
	}

	CGLPixelFormatAttribute attribs[] = {
		kCGLPFAColorSize, 24,
		kCGLPFAAlphaSize, ctx->alphaDepth,
		kCGLPFADepthSize, ctx->depthDepth,
		kCGLPFAStencilSize, ctx->stencilDepth,
		kCGLPFADoubleBuffer, true,
		kCGLPFASampleBuffers, ctx->multisamples > 0,
		kCGLPFASamples, ctx->multisamples,
		kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute) kCGLOGLPVersion_GL4_Core,
		0
	};

	CGLPixelFormatObj pxFmt = NULL;
	int numPxFmt = 1;
	CGLError err = CGLChoosePixelFormat(attribs, &pxFmt, &numPxFmt);
	if (!pxFmt || err != kCGLNoError) {
		rlawtThrowCGLError(env, "unable to choose format", err);
		goto freeDSI;
	}

	err = CGLCreateContext(pxFmt, NULL, &ctx->context);
	CGLReleasePixelFormat(pxFmt);
	if (!ctx->context || err != kCGLNoError) {
		rlawtThrowCGLError(env, "unable to create context", err);
		goto freeDSI;
	}

	if (!makeCurrent(env, ctx->context)) {
		goto freeContext;
	}

	glGenTextures(2, &ctx->tex[0]);
	glGenFramebuffers(2, &ctx->fbo[0]);

	dispatch_sync(dispatch_get_main_queue(), ^{
		RLLayer *layer = [[RLLayer alloc] init];
		layer.opaque = true;
		layer.needsDisplayOnBoundsChange = false;
		layer.magnificationFilter = kCAFilterNearest;
		layer.contentsGravity = kCAGravityCenter;
		layer.affineTransform = CGAffineTransformMakeScale(1, -1);

		ctx->layer = layer;
		dspi.layer = layer;

		// must be after we give jawt the layer so our frame fix works
		layer.frame = CGRectMake(
			dsi->bounds.x + ctx->offsetX,
			dspi.windowLayer.bounds.size.height - (dsi->bounds.y + ctx->offsetY) - dsi->bounds.height, // as per AWTSurfaceLayers::setBounds
			dsi->bounds.width,
			dsi->bounds.height);
	});

	if (!rlawtCreateIOSurface(env, ctx)) {
		goto freeContext;
	}

	ctx->ds->FreeDrawingSurfaceInfo(dsi);

	ctx->contextCreated = true;
	return;

freeContext:
	CGLDestroyContext(ctx->context);
freeDSI:
	ctx->ds->FreeDrawingSurfaceInfo(dsi);
}

void rlawtContextFreePlatform(JNIEnv *env, AWTContext *ctx) {
	CGLSetCurrentContext(NULL);
	if (ctx->context) {
		CGLDestroyContext(ctx->context);
	}
	if (ctx->buffer[0]) {
		CFRelease(ctx->buffer[0]);
	}
	if (ctx->buffer[1]) {
		CFRelease(ctx->buffer[1]);
	}
	if (ctx->layer) {
		dispatch_sync(dispatch_get_main_queue(), ^{
			[ctx->layer removeFromSuperlayer];
			[ctx->layer release];
		});
	}
}

JNIEXPORT int JNICALL Java_net_runelite_rlawt_AWTContext_setSwapInterval(JNIEnv *env, jobject self, jint interval) {
	return 0;
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_makeCurrent(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	makeCurrent(env, ctx->context);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_detachCurrent(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	makeCurrent(env, NULL);
}

JNIEXPORT void JNICALL Java_net_runelite_rlawt_AWTContext_swapBuffers(JNIEnv *env, jobject self) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return;
	}

	glFlush();
	dispatch_sync(dispatch_get_main_queue(), ^{
		[CATransaction begin];
		[CATransaction setDisableActions: true];
		ctx->layer.contents = (id) (ctx->buffer[ctx->back]);
		[(id<CanSetContentsChanged>)ctx->layer setContentsChanged];
		[(RLLayer*)ctx->layer fixFrame];
		[CATransaction commit];
	});
	ctx->back ^= 1;

	if (!ctx->buffer[ctx->back]
		|| IOSurfaceGetWidth(ctx->buffer[ctx->back]) != ctx->layer.frame.size.width 
		|| IOSurfaceGetHeight(ctx->buffer[ctx->back]) != ctx->layer.frame.size.height) {
		if (!rlawtCreateIOSurface(env, ctx)) {
			return;
		}
	}
}

JNIEXPORT jint JNICALL Java_net_runelite_rlawt_AWTContext_getFramebuffer(JNIEnv *env, jobject self, jboolean front) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

	return ctx->fbo[ctx->back ^ front];
}

#endif