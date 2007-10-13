/**
 * \file src/stream/gl_play.c
 * \brief OpenGL playback
 * \author Pyry Haulos <pyry.haulos@gmail.com>
 * \date 2007
 */

/* gl_play.c -- OpenGL stuff
 * Copyright (C) 2007 Pyry Haulos
 * For conditions of distribution and use, see copyright notice in glc.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <unistd.h>
#include <packetstream.h>
#include <pthread.h>
#include <errno.h>

#include "../common/glc.h"
#include "../common/util.h"
#include "../common/thread.h"
#include "gl_play.h"

/**
 * \addtogroup stream
 *  \{
 */

/**
 * \defgroup gl_play OpenGL playback
 *  \{
 */

struct gl_play_ctx_s {
	Display *dpy;
	GLXDrawable drawable;
	GLXContext ctx;
	char name[100];

	unsigned int w, h;
	float zoom;

	glc_ctx_i ctx_i;

	glc_utime_t last;

	int unsupported;
	int created;

	Atom delete_atom, wm_proto_atom;

	struct gl_play_ctx_s *next;
};

struct gl_play_private_s {
	glc_t *glc;
	Display *dpy;
	GLenum capture_buffer;
	glc_utime_t fps;

	struct gl_play_ctx_s *ctx;
	glc_ctx_i ctx_c;
	glc_ctx_i last_ctx;

	glc_thread_t play_thread;

	unsigned int bpp;
	GLenum format;
};

int gl_play_read_callback(glc_thread_state_t *state);
void gl_play_finish_callback(void *ptr, int err);

int gl_play_set_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx);
int gl_play_create_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx);
int gl_play_update_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx);
int gl_play_get_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s **ctx, glc_ctx_i ctx_i);

int gl_play_put_pixels(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx, char *from);

int gl_play_handle_xevents(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx);

int gl_play_init(glc_t *glc, ps_buffer_t *from)
{
	struct gl_play_private_s *gl_play = (struct gl_play_private_s *) malloc(sizeof(struct gl_play_private_s));
	memset(gl_play, 0, sizeof(struct gl_play_private_s));
	
	gl_play->glc = glc;
	gl_play->last_ctx = -1;
	
	gl_play->play_thread.flags = GLC_THREAD_READ;
	gl_play->play_thread.ptr = gl_play;
	gl_play->play_thread.read_callback = &gl_play_read_callback;
	gl_play->play_thread.finish_callback = &gl_play_finish_callback;
	gl_play->play_thread.threads = 1;
	
	gl_play->dpy = XOpenDisplay(NULL);
	
	if (!gl_play->dpy) {
		fprintf(stderr, "can't open display\n");
		return 1;
	}
	
	return glc_thread_create(glc, &gl_play->play_thread, from, NULL);
}

void gl_play_finish_callback(void *ptr, int err)
{
	struct gl_play_private_s *gl_play = (struct gl_play_private_s *) ptr;
	struct gl_play_ctx_s *del;
	
	if (err)
		fprintf(stderr, "gl failed: %s (%d)\n", strerror(err), err);

	while (gl_play->ctx != NULL) {
		del = gl_play->ctx;
		gl_play->ctx = gl_play->ctx->next;
		
		if (del->dpy != NULL) {
			glXDestroyContext(del->dpy, del->ctx);
			XUnmapWindow(del->dpy, del->drawable);
			XDestroyWindow(del->dpy, del->drawable);
		}
		free(del);
	}
	
	XCloseDisplay(gl_play->dpy);
	
	sem_post(&gl_play->glc->signal[GLC_SIGNAL_GL_PLAY_FINISHED]);
	free(gl_play);
}

int gl_play_put_pixels(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx, char *from)
{
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glBitmap(0, 0, 0, 0, 0, 0, NULL);
	
	if (ctx->zoom != 1.0)
		glPixelZoom(ctx->zoom, ctx->zoom);
	
	glDrawPixels(ctx->w, ctx->h, GL_BGR, GL_UNSIGNED_BYTE, from);
	glXSwapBuffers(ctx->dpy, ctx->drawable);

	return 0;
}

int gl_play_get_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s **ctx, glc_ctx_i ctx_i)
{
	struct gl_play_ctx_s *fctx = gl_play->ctx;
	
	while (fctx != NULL) {
		if (fctx->ctx_i == ctx_i)
			break;
		fctx = fctx->next;
	}
	
	if (fctx == NULL) {
		fctx = (struct gl_play_ctx_s *) malloc(sizeof(struct gl_play_ctx_s));
		memset(fctx, 0, sizeof(struct gl_play_ctx_s));
		
		fctx->next = gl_play->ctx;
		gl_play->ctx = fctx;
		fctx->ctx_i = ctx_i;
	}
	
	*ctx = fctx;
	
	return 0;
}

int gl_play_create_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx)
{
	int attribs[] = { GLX_RGBA,
			  GLX_RED_SIZE, 1,
			  GLX_GREEN_SIZE, 1,
			  GLX_BLUE_SIZE, 1,
			  GLX_DOUBLEBUFFER,
			  GLX_DEPTH_SIZE, 1,
			  None };
	XVisualInfo *visinfo;
	XSetWindowAttributes winattr;

	ctx->zoom = 1;
	ctx->dpy = gl_play->dpy;
	visinfo = glXChooseVisual(ctx->dpy, DefaultScreen(ctx->dpy), attribs);
	
	winattr.background_pixel = 0;
	winattr.border_pixel = 0;
	winattr.colormap = XCreateColormap(ctx->dpy, RootWindow(ctx->dpy, DefaultScreen(ctx->dpy)),
	                                   visinfo->visual, AllocNone);
	winattr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask | KeyReleaseMask;
	winattr.override_redirect = 0;
	ctx->drawable = XCreateWindow(ctx->dpy, RootWindow(ctx->dpy, DefaultScreen(ctx->dpy)),
	                              0, 0, ctx->w, ctx->h, 0, visinfo->depth, InputOutput,
	                              visinfo->visual, CWBackPixel | CWBorderPixel |
	                              CWColormap | CWEventMask | CWOverrideRedirect, &winattr);

	ctx->ctx = glXCreateContext(ctx->dpy, visinfo, NULL, True);
	ctx->created = 1;

	XFree(visinfo);

	ctx->delete_atom = XInternAtom(ctx->dpy, "WM_DELETE_WINDOW", False);
	ctx->wm_proto_atom = XInternAtom(ctx->dpy, "WM_PROTOCOLS", True);
	XSetWMProtocols(ctx->dpy, ctx->drawable, &ctx->delete_atom, 1);

	return gl_play_update_ctx(gl_play, ctx);
}

int gl_play_update_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx)
{
	XSizeHints sizehints;
	
	if (!ctx->created)
		return EINVAL;
	
	snprintf(ctx->name, sizeof(ctx->name) - 1, "glc-play (ctx %d)", ctx->ctx_i);
	
	ctx->zoom = 1; /* reset zoom, sorry */

	XUnmapWindow(ctx->dpy, ctx->drawable);
	
	sizehints.x = 0;
	sizehints.y = 0;
	sizehints.width = ctx->w;
	sizehints.height = ctx->h;
	sizehints.min_aspect.x = ctx->w;
	sizehints.min_aspect.y = ctx->h;
	sizehints.max_aspect.x = ctx->w;
	sizehints.max_aspect.y = ctx->h;
	sizehints.flags = USSize | USPosition | PAspect;
	XSetNormalHints(ctx->dpy, ctx->drawable, &sizehints);
	XSetStandardProperties(ctx->dpy, ctx->drawable, ctx->name, ctx->name, None,
	                       (char **)NULL, 0, &sizehints);
	XResizeWindow(ctx->dpy, ctx->drawable, ctx->w, ctx->h);

	XMapWindow(ctx->dpy, ctx->drawable);
	
	glXMakeCurrent(ctx->dpy, ctx->drawable, ctx->ctx);
	glViewport(0, 0, (GLsizei) ctx->w, (GLsizei) ctx->h);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	
	return 0;
}

int gl_play_set_ctx(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx)
{
	gl_play->dpy = ctx->dpy;
	glXMakeCurrent(ctx->dpy, ctx->drawable, ctx->ctx);
	
	return 0;
}

int gl_handle_xevents(struct gl_play_private_s *gl_play, struct gl_play_ctx_s *ctx)
{
	XEvent event;
	XConfigureEvent *ce;
	int code;

	while (XPending(gl_play->dpy) > 0) {
		XNextEvent(gl_play->dpy, &event);
		
		switch (event.type) {
		case KeyPress:
			code = XLookupKeysym(&event.xkey, 0);
			
			if (code == XK_Right)
				util_timediff(gl_play->glc, -100000);
			break;
		case KeyRelease:
			code = XLookupKeysym(&event.xkey, 0);

			if (code == XK_Escape)
				gl_play->glc->flags |= GLC_CANCEL;
			break;
		case DestroyNotify:
			gl_play->glc->flags |= GLC_CANCEL;
			break;
		case ClientMessage:
			if (event.xclient.message_type == ctx->wm_proto_atom) {
				if ((Atom) event.xclient.data.l[0] == ctx->delete_atom)
					gl_play->glc->flags |= GLC_CANCEL;
			}
			break;
		case ConfigureNotify:
			ce = (XConfigureEvent *) &event;
			ctx->zoom = (float) ce->width / (float) ctx->w;
			
			break;
		}
	}

	return 0;
}

int gl_play_read_callback(glc_thread_state_t *state)
{
	struct gl_play_private_s *gl_play = (struct gl_play_private_s *) state->ptr;
	
	glc_ctx_message_t *ctx_msg;
	glc_picture_header_t *pic_hdr;
	glc_utime_t time;
	struct gl_play_ctx_s *ctx;
	
	if (state->header.type == GLC_MESSAGE_CTX) {
		ctx_msg = (glc_ctx_message_t *) state->read_data;
		gl_play_get_ctx(gl_play, &ctx, ctx_msg->ctx);
		ctx->w = ctx_msg->w;
		ctx->h = ctx_msg->h;
		
		if ((ctx_msg->flags & GLC_CTX_BGR) && (ctx_msg->flags & GLC_CTX_CREATE)) {
			gl_play_create_ctx(gl_play, ctx);
			gl_play->last_ctx = ctx_msg->ctx;
		} else if ((ctx_msg->flags & GLC_CTX_BGR) && (ctx_msg->flags & GLC_CTX_UPDATE)) {
			if (gl_play_update_ctx(gl_play, ctx))
				fprintf(stderr, "broken ctx %d\n", ctx_msg->ctx);
		} else {
			ctx->unsupported = 1;
			printf("ctx %d is in unsupported format\n", ctx_msg->ctx);
		}
	} else if (state->header.type == GLC_MESSAGE_PICTURE) {
		pic_hdr = (glc_picture_header_t *) state->read_data;
		gl_play_get_ctx(gl_play, &ctx, pic_hdr->ctx);
		
		if (ctx->unsupported)
			return 0;
		
		if (!ctx->created) {
			fprintf(stderr, "picture refers to uninitalized ctx %d\n", pic_hdr->ctx);
			gl_play->glc->flags |= GLC_CANCEL;
			return EINVAL;
		}
		
		if (gl_play->last_ctx != pic_hdr->ctx) {
			gl_play_set_ctx(gl_play, ctx);
			gl_play->last_ctx = pic_hdr->ctx;
		}
		
		gl_handle_xevents(gl_play, ctx);
		
		time = util_timestamp(gl_play->glc);
		
		if (pic_hdr->timestamp > time)
			usleep(pic_hdr->timestamp - time);
		else if (time > pic_hdr->timestamp + gl_play->fps)
			return 0;

		gl_play_put_pixels(gl_play, ctx, &state->read_data[GLC_PICTURE_HEADER_SIZE]);
	}

	return 0;
}

/**  \} */
/**  \} */