#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <pixman.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <wayland-client.h>
#include "utf8.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;

static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_output *wl_output;
static struct wl_surface *wl_surface;

static int32_t output = -1;
static uint32_t width, height;
static uint32_t stride, bufsize;
static int on_top = 0; // on top of windows or not
static bool run_display = true;

static struct fcft_font *font;
static char line[128];
static char lastline[128];
static int linerem;

/* foreground text colors */
static pixman_color_t
	fgcolor = {
		.red = 0xb3b3,
		.green = 0xb3b3,
		.blue = 0xb3b3,
		.alpha = 0xffff,
	};

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
	/* sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static int allocate_shm_file(size_t size) {
  /* shared memory support */
	int fd = memfd_create("surface", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	return fd;
}

int max(int a, int b) {
   return (a > b) ? a : b;
}

static struct wl_buffer *draw_frame(char *text) {
	/* allocate buffer to be attached to the surface */
	int fd = allocate_shm_file(bufsize);
	if (fd == -1)
		return NULL;

	uint32_t *data = mmap(NULL, bufsize,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bufsize);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* premultiplied colors */
  pixman_color_t textfgcolor = fgcolor;

	/* Pixman image corresponding to main buffer */
	pixman_image_t *bar = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, data, width * 4);

	/* text background and foreground layers */
	pixman_image_t *background = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, NULL, width * 4);

  pixman_image_t *foreground = pixman_image_create_bits(PIXMAN_a8r8g8b8,
			width, height, NULL, width * 4);

	pixman_image_t *fgfill = pixman_image_create_solid_fill(&textfgcolor);

	/* start drawing at center-left (ypos sets the text baseline) */
	uint32_t xpos = 0, maxxpos = 0;
	uint32_t ypos = (height + font->ascent - font->descent) / 2;

	uint32_t codepoint, lastcp = 0, state = UTF8_ACCEPT;
	for (char *p = text; *p; p++) {
		/* check for inline ^ commands */
		if (state == UTF8_ACCEPT && *p == '^') p++; 

		/* return nonzero if more bytes are needed */
		if (utf8decode(&state, &codepoint, *p)) continue;

    /* rasterize a glyph for a single UTF-32 codepoint */
		const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(font, codepoint,
				FCFT_SUBPIXEL_DEFAULT);
		if (!glyph) continue;

		/* adjust x position based on kerning with previous glyph */
		long x_kern = 0;
		if (lastcp)
			fcft_kerning(font, lastcp, codepoint, &x_kern, NULL);
		xpos += x_kern;
		lastcp = codepoint;

    /* apply foreground for subpixel rendered text */
 		pixman_image_composite32(
 			PIXMAN_OP_OVER, fgfill, glyph->pix, foreground, 0, 0, 0, 0,
 			xpos + glyph->x, ypos - glyph->y, glyph->width, glyph->height);

		/* increment pen position */
		xpos += glyph->advance.x;
		ypos += glyph->advance.y;
		maxxpos = max(maxxpos, xpos);
	}
	pixman_image_unref(fgfill);

	if (state != UTF8_ACCEPT)
		fprintf(stderr, "malformed UTF-8 sequence\n");

	/* make it colorful */
	int32_t xdraw = (width - maxxpos) / 2;
	pixman_image_composite32(PIXMAN_OP_OVER, background, NULL, bar, 0, 0, 0, 0,
			xdraw, 0, width, height);
	pixman_image_composite32(PIXMAN_OP_OVER, foreground, NULL, bar, 0, 0, 0, 0,
			xdraw, 0, width, height);

	pixman_image_unref(foreground);
	pixman_image_unref(background);
	pixman_image_unref(bar);
	munmap(data, bufsize);
	return buffer;
}

/* layer-surface setup adapted from wlroots */
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t w, uint32_t h) {
	width = w;
	height = h;
	stride = width * 4;
	bufsize = stride * height;

	if (on_top > 0)
		on_top = height;

	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, on_top);
	zwlr_layer_surface_v1_ack_configure(surface, serial);

	struct wl_buffer *buffer = draw_frame(lastline);
	if (!buffer)
		return;
	wl_surface_attach(wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(wl_surface, 0, 0, width, height);
	wl_surface_commit(wl_surface);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	zwlr_layer_surface_v1_destroy(surface);
	wl_surface_destroy(wl_surface);
	run_display = false;
}

static struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output *o = wl_registry_bind(registry, name,
				&wl_output_interface, 1);
		if (output-- == 0)
			wl_output = o;
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name,
				&zwlr_layer_shell_v1_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {.global = handle_global,};

static void read_clock(void) {
	/* load UNIX date to line buffer */
  FILE *fp = popen("date", "r");
  
  while (fgets(line + linerem, 128 - linerem, fp) != NULL) {
     linerem += strlen(line + linerem);
  }
  
  pclose(fp);

	/* process the lines in the buffer sequentially */
	char *curline, *end;
	struct wl_buffer *buffer = NULL;

  for (curline = line; (end = memchr(curline, '\n', linerem)); curline = end) {
  	*end++ = '\0';
  	linerem -= end - curline;
  
  	/* redraw last line when needed */
  	memcpy(lastline, curline, end - curline);
  
  	if (!(buffer = draw_frame(lastline)))
  		continue;
  }

	if (linerem == 128) {
		/* discard the line when buffer is full */
		linerem = 0;
	} else if (linerem && curline != line) {
		/* shift remaining data */
		memmove(line, curline, linerem);
	}

	/* redraw */
	if (buffer) {
		wl_surface_attach(wl_surface, buffer, 0, 0);
		wl_surface_damage_buffer(wl_surface, 0, 0, width, height);
		wl_surface_commit(wl_surface);
	}
}

static void event_loop(void) {
	int wlfd = wl_display_get_fd(display);

	while (run_display) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(wlfd, &rfds);
		wl_display_flush(display);

		read_clock();

		if (FD_ISSET(wlfd, &rfds))
			if (wl_display_dispatch(display) == -1) break;
	}
}


int main(int argc, char **argv) {
	char *namespace = "bruh";
	char *fontstr = "BigBlueTermPlus Nerd Font Mono:size=11";
	uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP; // *_BOTTOM is also fine  

	/* set up display and protocols */
	display = wl_display_connect(NULL);

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	/* load font font */
	fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	font = fcft_from_name(1, (const char *[]) {fontstr}, NULL);

	/* make layer-shell surface */
	wl_surface = wl_compositor_create_surface(compositor);

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
			wl_surface, wl_output, layer, namespace);
	zwlr_layer_surface_v1_add_listener(layer_surface,
			&layer_surface_listener, layer_surface);

	/* layer size and positioning */
	if (!height)
		height = font->ascent + font->descent;

	zwlr_layer_surface_v1_set_size(layer_surface, width, height);
	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
	wl_surface_commit(wl_surface);
	wl_display_roundtrip(display);

	event_loop();

	/* cleanup */
	zwlr_layer_surface_v1_destroy(layer_surface);
	wl_surface_destroy(wl_surface);
	zwlr_layer_shell_v1_destroy(layer_shell);
	fcft_destroy(font);
	fcft_fini();
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	return 0;
}
