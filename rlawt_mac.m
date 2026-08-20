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
	@public int offsetY;
	CGFloat newScale;
}
- (void)setFrame:(CGRect)newValue;
- (void)fixFrame;
- (void)displayIOSurface:(id)ioSurface;
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

- (void)displayIOSurface:(id)ioSurface {
	[CATransaction begin];
	[CATransaction setDisableActions: true];
	self.contentsScale = self->newScale;
	self.contents = ioSurface;
	[(id<CanSetContentsChanged>)self setContentsChanged];
	[self fixFrame];
	[CATransaction commit];
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

// -- IOSurfacePool helpers --

#ifdef RLAWT_POOL_DEBUG
#define POOL_STAT(pool, field) ((pool)->stats.field++)
#else
#define POOL_STAT(pool, field) ((void)0)
#endif

static void poolInit(IOSurfacePool *pool) {
	memset(pool, 0, sizeof(*pool));
	GLuint textures[RLAWT_MAX_POOL], framebuffers[RLAWT_MAX_POOL];
	glGenTextures(RLAWT_MAX_POOL, textures);
	glGenFramebuffers(RLAWT_MAX_POOL, framebuffers);
	for (int i = 0; i < RLAWT_MAX_POOL; i++) {
		pool->entries[i].tex = textures[i];
		pool->entries[i].fbo = framebuffers[i];
	}
#ifdef RLAWT_POOL_DEBUG
	pool->lastLogTime = CFAbsoluteTimeGetCurrent();
#endif
}

static void poolDestroy(IOSurfacePool *pool) {
	for (int i = 0; i < RLAWT_MAX_POOL; i++) {
		glDeleteTextures(1, &pool->entries[i].tex);
		glDeleteFramebuffers(1, &pool->entries[i].fbo);
		if (pool->entries[i].surface) {
			CFRelease(pool->entries[i].surface);
		}
	}
}

static bool poolEntryMatchesSize(IOSurfaceEntry *entry, CALayer *layer) {
	if (!entry->surface) return false;
	CGFloat scale = entry->scale;
	CGSize size = layer.frame.size;
	return IOSurfaceGetWidth(entry->surface) == (size_t)(size.width * scale)
		&& IOSurfaceGetHeight(entry->surface) == (size_t)(size.height * scale)
		&& layer.superlayer.contentsScale == scale;
}

static bool poolCreateSurface(JNIEnv *env, IOSurfaceEntry *entry, CALayer *layer, CGLContextObj cglCtx) {
	CGFloat scale = layer.superlayer.contentsScale;
	CGSize size = layer.frame.size;
	size.width *= scale;
	size.height *= scale;

	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
	glBindTexture(target, entry->tex);
	CGLError err = CGLTexImageIOSurface2D(
		cglCtx,
		target, GL_RGBA,
		size.width, size.height,
		GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
		buf,
		0);
	glBindTexture(target, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, entry->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, entry->tex, 0);

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

	if (entry->surface) {
		CFRelease(entry->surface);
	}
	entry->surface = buf;
	entry->scale = scale;
	return true;
freeSurface:
	CFRelease(buf);
	return false;
}

static int poolNextBack(IOSurfacePool *pool) {
	// Find any non-front surface the compositor isn't using and which is not queued
	// for asynchronous presentation on the AppKit main thread.
	for (int i = 0; i < pool->count; i++) {
		if (i == pool->front) continue;
		if (pool->entries[i].surface && !pool->entries[i].queued && !IOSurfaceIsInUse(pool->entries[i].surface)) {
			POOL_STAT(pool, reuses);
			return i;
		}
	}

	// Grow pool if nothing was available
	if (pool->count < RLAWT_MAX_POOL) {
		int idx = pool->count++;
		POOL_STAT(pool, grows);
#ifdef RLAWT_POOL_DEBUG
		if (pool->count > pool->stats.highWater) {
			pool->stats.highWater = pool->count;
		}
#endif
		return idx;
	}

	// Pool full, all in use -- evict first non-front entry that is not queued
	// for async presentation.
	for (int i = 0; i < pool->count; i++) {
		if (i != pool->front && !pool->entries[i].queued) {
			POOL_STAT(pool, stalls);
			return i;
		}
	}

	POOL_STAT(pool, stalls);
	return -1;
}

static bool poolCanPresentWithoutBlocking(IOSurfacePool *pool, int nextFront) {
	for (int i = 0; i < pool->count; i++) {
		if (i == nextFront) continue;
		if (pool->entries[i].surface && !pool->entries[i].queued && !IOSurfaceIsInUse(pool->entries[i].surface)) {
			return true;
		}
	}

	if (pool->count < RLAWT_MAX_POOL) {
		return true;
	}

	for (int i = 0; i < pool->count; i++) {
		if (i != nextFront && !pool->entries[i].queued) {
			return true;
		}
	}

	return false;
}

#ifdef RLAWT_POOL_DEBUG
static void poolLogStats(IOSurfacePool *pool) {
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (now - pool->lastLogTime < 60.0) return;

	NSLog(@"IOSurfacePool: %d swaps, %d allocs, pool %d/%d (hw %d), %d reuse, %d grow, %d stall, %d fail",
		pool->stats.swaps,
		pool->stats.allocs,
		pool->count,
		RLAWT_MAX_POOL,
		pool->stats.highWater,
		pool->stats.reuses,
		pool->stats.grows,
		pool->stats.stalls,
		pool->stats.failures);

	memset(&pool->stats, 0, sizeof(pool->stats));
	pool->stats.highWater = pool->count;
	pool->lastLogTime = now;
}
#endif

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

	poolInit(&ctx->pool);

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

	ctx->pool.count = 1;
	if (!poolCreateSurface(env, &ctx->pool.entries[0], ctx->layer, ctx->context)) {
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
	poolDestroy(&ctx->pool);
	CGLSetCurrentContext(NULL);
	if (ctx->context) {
		CGLDestroyContext(ctx->context);
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

	IOSurfacePool *pool = &ctx->pool;

#ifdef RLAWT_ASYNC_PRESENT
	if (!poolCanPresentWithoutBlocking(pool, pool->back)) {
		POOL_STAT(pool, stalls);
		return;
	}
#endif

	// Present the current back buffer
	pool->front = pool->back;
	RLLayer *rlLayer = (RLLayer*) ctx->layer;
	rlLayer->newScale = pool->entries[pool->front].scale;
	IOSurfaceEntry *frontEntry = &pool->entries[pool->front];
	IOSurfaceRef frontSurface = frontEntry->surface;

#ifdef RLAWT_ASYNC_PRESENT
	frontEntry->queued = true;
	CFRetain(frontSurface);
	[rlLayer retain];
	dispatch_async(dispatch_get_main_queue(), ^{
		[rlLayer displayIOSurface:(id)frontSurface];
		frontEntry->queued = false;
		CFRelease(frontSurface);
		[rlLayer release];
	});
#else
	[rlLayer performSelectorOnMainThread:
		@selector(displayIOSurface:)
		withObject: (id)frontSurface
		waitUntilDone: true];
#endif

	POOL_STAT(pool, swaps);

	// Find a free surface for the next frame
	int nextBack = poolNextBack(pool);
	if (nextBack < 0) {
		return;
	}
	pool->back = nextBack;
	IOSurfaceEntry *back = &pool->entries[pool->back];

	// Create/recreate surface if needed (new slot, wrong size, or still held by compositor)
	if (!poolEntryMatchesSize(back, ctx->layer)
		|| IOSurfaceIsInUse(back->surface)) {
		if (!poolCreateSurface(env, back, ctx->layer, ctx->context)) {
			POOL_STAT(pool, failures);
			return;
		}
		POOL_STAT(pool, allocs);
	}

#ifdef RLAWT_POOL_DEBUG
	poolLogStats(pool);
#endif
}

JNIEXPORT jint JNICALL Java_net_runelite_rlawt_AWTContext_getFramebuffer(JNIEnv *env, jobject self, jboolean front) {
	AWTContext *ctx = rlawtGetContext(env, self);
	if (!ctx || !rlawtContextState(env, ctx, true)) {
		return 0;
	}

	if (front) {
		return ctx->pool.entries[ctx->pool.front].fbo;
	}
	return ctx->pool.entries[ctx->pool.back].fbo;
}

#endif
