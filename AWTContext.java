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
package net.runelite.rlawt;

import java.awt.Component;
import java.awt.Insets;
import java.awt.Window;
import java.io.IOException;
import java.io.InputStream;
import java.lang.annotation.Native;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

public final class AWTContext
{
	private static boolean nativesLoaded = false;

	@Native
	private long instance;

	public synchronized static void loadNatives()
	{
		if (nativesLoaded)
		{
			return;
		}

		// this needs to be in core since doing this in hub plugins will break core gpu
		System.loadLibrary("jawt");

		String overridePath = System.getProperty("runelite.rlawtpath");
		if (overridePath != null)
		{
			System.load(overridePath);
			nativesLoaded = true;
			return;
		}

		String os = System.getProperty("os.name", "no-os").toLowerCase();
		String arch = System.getProperty("os.arch", "no-arch");
		String name = "unknown";
		if (os.contains("mac") || os.contains("darwin"))
		{
			os = "macos";
			name = "librlawt.dylib";
		}
		else if (os.contains("win"))
		{
			os = "windows";
			name = "rlawt.dll";
		}
		else if (os.contains("nux"))
		{
			os = "linux";
			name = "librlawt.so";
		}

		String path = os + "-" + arch + "/" + name;
		try (InputStream is = AWTContext.class.getResourceAsStream(path))
		{
			if (is == null)
			{
				throw new RuntimeException("rlawt does not exist at " + path);
			}

			Path temp = Files.createTempFile("", name);
			temp.toFile().deleteOnExit();
			Files.copy(is, temp, StandardCopyOption.REPLACE_EXISTING);
			System.load(temp.toAbsolutePath().toString());
			nativesLoaded = true;
		}
		catch (IOException e)
		{
			throw new RuntimeException(e);
		}
	}

	private static native long create0(Component component);

	public AWTContext(Component component)
	{
		this.instance = create0(component);
		if (instance == 0)
		{
			throw new NullPointerException();
		}

		// JAWT on osx does not set our bounds when rlawt creates the CALayer
		// so we have to calculate it's offset from the superlayer until it is first
		// resized. We assume that the superlayer is the Window
		int x = 0;
		int y = 0;
		for (Component c = component.getParent(); c != null; c = c.getParent())
		{
			if (c instanceof Window)
			{
				Insets insets = ((Window)c).getInsets();
				x -= insets.left;
				y -= insets.top;
				break;
			}

			x += c.getX();
			y += c.getY();
		}
		configureInsets(x, y);
	}

	public native void destroy();

	private native void configureInsets(int x, int y);

	public native void configurePixelFormat(int alpha, int depth, int stencil);

	public native void configureMultisamples(int samples);

	/**
	 * Gets the name of the active front or back framebuffer object.
	 */
	public native int getFramebuffer(boolean front);

	/**
	 * Gets the framebuffer target name associated with {@link #getFramebuffer(boolean)}
	 */
	public int getBufferMode()
	{
		final int GL_FRONT = 0x404;
		final int GL_COLOR_ATTACHMENT0 = 0x8CE0;

		return getFramebuffer(true) == 0 ? GL_FRONT : GL_COLOR_ATTACHMENT0;
	}

	/**
	 * Ends the configuration phase of the context, actually creating an OpenGL
	 * context.
	 */
	public native void createGLContext();

	public native int setSwapInterval(int interval);

	public native void makeCurrent();

	public native void detachCurrent();

	/**
	 * Presents the framebuffer to the user. After calling this you MUST bind
	 * the current active framebuffer (see {@link #getFramebuffer(boolean)}) before drawing anything else
	 */
	public native void swapBuffers();

	public native long getGLContext();

	public native long getCGLShareGroup();

	public native long getGLXDisplay();

	public native long getWGLHDC();

}
