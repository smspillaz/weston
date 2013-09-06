/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

#include <wayland-client.h>
#include "../src/composer-client-protocol.h"
#include "../shared/os-compatibility.h"

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shell *shell;
	struct wl_shm *shm;
	struct composer_animation_controller *composer;
	uint32_t formats;
};

struct buffer {
	struct wl_buffer *buffer;
	void *shm_data;

	struct composer_buffer *cbuffer;
	void *cshm_data;

	int busy;
};

struct window {
	struct display *display;
	int width, height;
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	struct composer_surface *csurface;
	struct buffer buffers[2];
	struct buffer *prev_buffer;
	struct wl_callback *callback;
};

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

/* Returns fd */
static int
create_mmaped_file(struct display *display,
		   size_t size,
		   void **data)
{
	int fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n",
			(int) size);
		return -1;
	}

	*data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return -1;
	}

	return fd;
}

static int
create_shm_buffer_image(struct display *display, struct buffer *buffer,
			int width, int height, uint32_t format)
{
	struct wl_shm_pool *pool;
	int fd, size, stride;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = create_mmaped_file(display, size, &data);
	if (fd < 0)
		return -1;

	pool = wl_shm_create_pool(display->shm, fd, size);
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0,
						   width, height,
						   stride, format);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	buffer->shm_data = data;

	return 0;
}

static void sshm_buffer_release (void *data,
				 struct composer_buffer *cbuffer)
{
	struct buffer *buffer = data;
	(void) buffer;
}

static const struct composer_buffer_listener sshm_composer_buffer_listener =
{
	sshm_buffer_release
};

static int
create_shm_buffer_data(struct display *display, struct buffer *buffer,
		       size_t size)
{
	void *data = NULL;
	int fd = create_mmaped_file(display, size, &data);

	if (fd < 0)
		return -1;

	struct composer_shm_pool *pool = composer_animation_controller_create_shm_pool(display->composer,
										       fd, size);
	buffer->cbuffer = composer_shm_pool_create_buffer (pool, size, 0);
	buffer->cshm_data = data;

	composer_buffer_add_listener(buffer->cbuffer, &sshm_composer_buffer_listener, buffer);
	composer_shm_pool_destroy(pool);
	close (fd);

	return 0;
}

static void
handle_ping(void *data, struct wl_shell_surface *shell_surface,
							uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
handle_configure(void *data, struct wl_shell_surface *shell_surface,
		 uint32_t edges, int32_t width, int32_t height)
{
}

static void
handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	handle_ping,
	handle_configure,
	handle_popup_done
};

static struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);
	window->shell_surface = wl_shell_get_shell_surface(display->shell,
							   window->surface);

	if (window->shell_surface)
		wl_shell_surface_add_listener(window->shell_surface,
					      &shell_surface_listener, window);

	wl_shell_surface_set_title(window->shell_surface, "simple-shm");

	wl_shell_surface_set_toplevel(window->shell_surface);

	window->csurface =
		composer_animation_controller_get_composer_surface_animation_controller(display->composer,
											window->surface);

	static const char vertex_shader[] =
		"uniform mat4 proj;\n"
		"attribute vec2 position;\n"
		"attribute vec2 texcoord;\n"
		"varying vec2 v_texcoord;\n"
		"void main()\n"
		"{\n"
		"    gl_Position = proj * vec4(position, 0.0, 1.0);\n"
		"    v_texcoord = texcoord;\n"
		"}\n";

	static const char fragment_shader[] =
		"precision mediump float;\n"
		"varying vec2 v_texcoord;\n"
		"uniform sampler2D tex;\n"
		"uniform float alpha;\n"
		"uniform vec2 size;\n"
		"void main()\n"
		"{\n"
		"    vec2 norm_size = vec2(1.0 / size.x, 1.0 / size.y);\n"
		"    vec4 s1 = texture2D(tex, v_texcoord + vec2(norm_size.x * 1.0, norm_size.y));\n"
		"    vec4 s2 = texture2D(tex, v_texcoord + vec2(norm_size.x * 2.0, norm_size.y));\n"
		"    vec4 s3 = texture2D(tex, v_texcoord + vec2(norm_size.x * 3.0, norm_size.y));\n"
		"    vec4 s4 = texture2D(tex, v_texcoord + vec2(norm_size.x, norm_size.y));\n"
		"    vec4 s5 = texture2D(tex, v_texcoord + vec2(norm_size.x * -1.0, norm_size.y));\n"
		"    vec4 s6 = texture2D(tex, v_texcoord + vec2(norm_size.x * -2.0, norm_size.y));\n"
		"    vec4 s7 = texture2D(tex, v_texcoord + vec2(norm_size.x * -3.0, norm_size.y));\n"
		"    vec4 color = s3 * 0.05 + s2 * 0.10 + s1 * 0.2 + s4 * 0.30 + s7 * 0.05 + s6 * 0.10 + s5 * 0.20;\n"
		"    gl_FragColor = alpha * color;\n"
		"}\n";

	composer_surface_override_vertex_shader(window->csurface,
						vertex_shader,
						strlen (vertex_shader));
	composer_surface_override_fragment_shader(window->csurface,
						  fragment_shader,
						  strlen (fragment_shader));
	composer_surface_attach_uniform(window->csurface,
					"proj",
					COMPOSER_SURFACE_VARIABLE_SIZE_MAT4);
	composer_surface_attach_uniform(window->csurface,
					"alpha",
					COMPOSER_SURFACE_VARIABLE_SIZE_FLOAT);
	composer_surface_attach_uniform(window->csurface,
					"size",
					COMPOSER_SURFACE_VARIABLE_SIZE_VEC2);
	composer_surface_attach_vertex_attribute(window->csurface,
						 "position",
						 COMPOSER_SURFACE_VARIABLE_SIZE_VEC2);
	composer_surface_attach_vertex_attribute(window->csurface,
						 "texcoord",
						 COMPOSER_SURFACE_VARIABLE_SIZE_VEC2);

	return window;
}

static void
destroy_window(struct window *window)
{
	if (window->callback)
		wl_callback_destroy(window->callback);

	if (window->buffers[0].buffer)
		wl_buffer_destroy(window->buffers[0].buffer);
	if (window->buffers[1].buffer)
		wl_buffer_destroy(window->buffers[1].buffer);

	wl_shell_surface_destroy(window->shell_surface);
	wl_surface_destroy(window->surface);
	free(window);
}

static struct buffer *
window_next_buffer(struct window *window)
{
	struct buffer *buffer;
	int ret = 0;

	if (!window->buffers[0].busy)
		buffer = &window->buffers[0];
	else if (!window->buffers[1].busy)
		buffer = &window->buffers[1];
	else
		return NULL;

	if (!buffer->buffer) {
		ret = create_shm_buffer_image(window->display, buffer,
					      window->width, window->height,
					      WL_SHM_FORMAT_XRGB8888);

		if (ret < 0)
			return NULL;

		/* paint the padding */
		memset(buffer->shm_data, 0xff,
		       window->width * window->height * 4);
	}

	if (!buffer->cbuffer) {
		size_t required_size = 0;

		required_size += 4 * 4 * sizeof (float); // proj
		required_size += sizeof (float); // alpha
		required_size += sizeof (float) * 2; // size

		size_t i = 0;
		for (; i < 4; ++i)
		{
			required_size += 2 * sizeof (float); // position
			required_size += 2 * sizeof (float); // texcoord
		}

		ret = create_shm_buffer_data(window->display,
					     buffer,
					     required_size);
	}

	return buffer;
}

static void
paint_pixels(void *image, int padding, int width, int height, uint32_t time)
{
	const int halfh = padding + (height - padding * 2) / 2;
	const int halfw = padding + (width  - padding * 2) / 2;
	int ir, or;
	uint32_t *pixel = image;
	int y;

	/* squared radii thresholds */
	or = (halfw < halfh ? halfw : halfh) - 8;
	ir = or - 32;
	or *= or;
	ir *= ir;

	pixel += padding * width;
	for (y = padding; y < height - padding; y++) {
		int x;
		int y2 = (y - halfh) * (y - halfh);

		pixel += padding;
		for (x = padding; x < width - padding; x++) {
			uint32_t v;

			/* squared distance from center */
			int r2 = (x - halfw) * (x - halfw) + y2;

			if (r2 < ir)
				v = (r2 / 32 + time / 64) * 0x0080401;
			else if (r2 < or)
				v = (y + time / 32) * 0x0080401;
			else
				v = (x + time / 16) * 0x0080401;
			v &= 0x00ffffff;

			/* cross if compositor uses X from XRGB as alpha */
			if (abs(x - y) > 6 && abs(x + y - height) > 6)
				v |= 0xff000000;

			*pixel++ = v;
		}

		pixel += padding;
	}
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *window = data;
	struct buffer *buffer;

	buffer = window_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
			!callback ? "Failed to create the first buffer.\n" :
			"Both buffers busy at redraw(). Server bug?\n");
		abort();
	}

	/* proj */
	void *cdata = buffer->cshm_data;
	float *fbuffer = (float *) cdata;
	fbuffer[0] = 1.0f;
	fbuffer[1] = 0.0f;
	fbuffer[2] = 0.0f;
	fbuffer[3] = 0.0f;

	fbuffer[4] = 0.0f;
	fbuffer[5] = 1.0f;
	fbuffer[6] = 0.0f;
	fbuffer[7] = 0.0f;

	fbuffer[8] = 0.0f;
	fbuffer[9] = 0.0f;
	fbuffer[10] = 1.0f;
	fbuffer[11] = 0.0f;

	fbuffer[12] = 0.0f;
	fbuffer[13] = 0.0f;
	fbuffer[14] = 0.0f;
	fbuffer[15] = 1.0f;

	cdata += sizeof (float) * 4 * 4;

	/* alpha */
	fbuffer = (float *) cdata;
	fbuffer[0] = 0.5f;

	cdata += sizeof (float);

	/* size */
	fbuffer = (float *) cdata;
	fbuffer[0] = window->width;
	fbuffer[1] = window->height;

	cdata += sizeof (float) * 2;

	struct buf_pnt {
		float x;
		float y;
	};

	struct buf_pnt pos_pnts[] =
	{
		{ 0.0f, 0.0f },
		{ 0.0f, (float) window->height },
		{ (float) window->width, 0.0f },
		{ (float) window->width, (float) window->height }
	};

	struct buf_pnt tc_pnts[] =
	{
		{ 0.0f, 0.0f },
		{ 0.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 1.0f, 1.0f }
	};

	size_t i = 0;
	for (; i < 4; ++i)
	{
		/* pos */
		fbuffer = (float *) cdata;
		fbuffer[0] = pos_pnts[i].x;
		fbuffer[1] = pos_pnts[i].y;

		cdata += sizeof (float) * 2;

		/* texcoord */
		fbuffer = (float *) cdata;
		fbuffer[0] = tc_pnts[i].x;
		fbuffer[1] = tc_pnts[i].y;

		cdata += sizeof (float) * 2;
	}

	paint_pixels(buffer->shm_data, 20, window->width, window->height, time);

	composer_surface_attach_data_buffer (window->csurface, buffer->cbuffer);
	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface,
			  20, 20, window->width - 40, window->height - 40);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);
	buffer->busy = 1;
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct display *d = data;

	d->formats |= (1 << format);
}

struct wl_shm_listener shm_listener = {
	shm_format
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry,
					 id, &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_shell") == 0) {
		d->shell = wl_registry_bind(registry,
					    id, &wl_shell_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry,
					  id, &wl_shm_interface, 1);
		wl_shm_add_listener(d->shm, &shm_listener, d);
	} else if (strcmp(interface, "composer_animation_controller") == 0) {
		d->composer = wl_registry_bind(registry,
					       id,
					       &composer_animation_controller_interface,
					       1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static struct display *
create_display(void)
{
	struct display *display;

	display = malloc(sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	display->display = wl_display_connect(NULL);
	assert(display->display);

	display->formats = 0;
	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
				 &registry_listener, display);
	wl_display_roundtrip(display->display);
	if (display->shm == NULL) {
		fprintf(stderr, "No wl_shm global\n");
		exit(1);
	}

	if (display->composer == NULL) {
		fprintf(stderr, "No composer global\n");
		exit(1);
	}

	wl_display_roundtrip(display->display);

	if (!(display->formats & (1 << WL_SHM_FORMAT_XRGB8888))) {
		fprintf(stderr, "WL_SHM_FORMAT_XRGB32 not available\n");
		exit(1);
	}

	wl_display_get_fd(display->display);
	
	return display;
}

static void
destroy_display(struct display *display)
{
	if (display->shm)
		wl_shm_destroy(display->shm);

	if (display->shell)
		wl_shell_destroy(display->shell);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}

static int running = 1;

static void
signal_int(int signum)
{
	running = 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct display *display;
	struct window *window;
	int ret = 0;

	display = create_display();
	window = create_window(display, 250, 250);
	if (!window)
		return 1;

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	/* Initialise damage to full surface, so the padding gets painted */
	wl_surface_damage(window->surface, 0, 0,
			  window->width, window->height);

	redraw(window, NULL, 0);

	while (running && ret != -1)
		ret = wl_display_dispatch(display->display);

	fprintf(stderr, "simple-shm exiting\n");
	destroy_window(window);
	destroy_display(display);

	return 0;
}
